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
/* What the Windows SDK's d3d12video.h has and mingw's does not: the feature structures of the video device, the decode statistics query, the *1 decode and process
 * arguments, and the later interfaces (ID3D12VideoDecoder1, DecoderHeap1, Processor1, DecodeCommandList2 / 3, ProcessCommandList1 / 2 / 3). Laid out as Microsoft's
 * DirectX-Headers (MIT) define them; the vtables in widl's C form, which scripts/gen-dx-vtables.py reads. Included after <d3d12video.h>. */
#pragma once

typedef enum D3D12_VIDEO_DECODE_STATUS {
  D3D12_VIDEO_DECODE_STATUS_OK = 0, D3D12_VIDEO_DECODE_STATUS_CONTINUE = 1, D3D12_VIDEO_DECODE_STATUS_CONTINUE_SKIP_DISPLAY = 2, D3D12_VIDEO_DECODE_STATUS_RESTART = 3,
  D3D12_VIDEO_DECODE_STATUS_RATE_EXCEEDED = 4
} D3D12_VIDEO_DECODE_STATUS;
typedef enum D3D12_VIDEO_DECODE_CONVERSION_SUPPORT_FLAGS { D3D12_VIDEO_DECODE_CONVERSION_SUPPORT_FLAG_NONE = 0, D3D12_VIDEO_DECODE_CONVERSION_SUPPORT_FLAG_SUPPORTED = 0x1 } D3D12_VIDEO_DECODE_CONVERSION_SUPPORT_FLAGS;
typedef enum D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT {
  D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_Y = 0, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_U = 1, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_V = 2,
  D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_R = 0, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_G = 1, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_B = 2,
  D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_A = 3
} D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT;
typedef enum D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAGS {
  D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_NONE = 0, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_Y = 1, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_U = 2,
  D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_V = 4, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_R = 1, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_G = 2,
  D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_B = 4, D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_A = 8
} D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAGS;
typedef enum D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAGS { D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAG_NONE = 0, D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAG_SUPPORTED = 0x1 } D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAGS;

typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT { UINT NodeIndex; UINT ProfileCount; } D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES { UINT NodeIndex; UINT ProfileCount; GUID *pProfiles; } D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT { UINT NodeIndex; D3D12_VIDEO_DECODE_CONFIGURATION Configuration; UINT FormatCount; } D3D12_FEATURE_DATA_VIDEO_DECODE_FORMAT_COUNT;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS { UINT NodeIndex; D3D12_VIDEO_DECODE_CONFIGURATION Configuration; UINT FormatCount; DXGI_FORMAT *pOutputFormats; } D3D12_FEATURE_DATA_VIDEO_DECODE_FORMATS;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_HISTOGRAM {
  UINT NodeIndex; GUID DecodeProfile; UINT Width; UINT Height; DXGI_FORMAT DecodeFormat; D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAGS Components; UINT BinCount; UINT CounterBitDepth;
} D3D12_FEATURE_DATA_VIDEO_DECODE_HISTOGRAM;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_CONVERSION_SUPPORT {
  UINT NodeIndex; D3D12_VIDEO_DECODE_CONFIGURATION Configuration; D3D12_VIDEO_SAMPLE DecodeSample; D3D12_VIDEO_FORMAT OutputFormat; DXGI_RATIONAL FrameRate; UINT BitRate;
  D3D12_VIDEO_DECODE_CONVERSION_SUPPORT_FLAGS SupportFlags; D3D12_VIDEO_SCALE_SUPPORT ScaleSupport;
} D3D12_FEATURE_DATA_VIDEO_DECODE_CONVERSION_SUPPORT;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE { D3D12_VIDEO_DECODER_HEAP_DESC VideoDecoderHeapDesc; UINT64 MemoryPoolL0Size; UINT64 MemoryPoolL1Size; } D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE1 { D3D12_VIDEO_DECODER_HEAP_DESC VideoDecoderHeapDesc; BOOL Protected; UINT64 MemoryPoolL0Size; UINT64 MemoryPoolL1Size; } D3D12_FEATURE_DATA_VIDEO_DECODER_HEAP_SIZE1;
typedef struct D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE {
  UINT NodeMask; const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *pOutputStreamDesc; UINT NumInputStreamDescs; const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *pInputStreamDescs;
  UINT64 MemoryPoolL0Size; UINT64 MemoryPoolL1Size;
} D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE;
typedef struct D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE1 {
  UINT NodeMask; const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *pOutputStreamDesc; UINT NumInputStreamDescs; const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *pInputStreamDescs;
  BOOL Protected; UINT64 MemoryPoolL0Size; UINT64 MemoryPoolL1Size;
} D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE1;
typedef struct D3D12_FEATURE_DATA_VIDEO_PROCESS_MAX_INPUT_STREAMS { UINT NodeIndex; UINT MaxInputStreams; } D3D12_FEATURE_DATA_VIDEO_PROCESS_MAX_INPUT_STREAMS;
typedef struct D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO {
  UINT NodeIndex; D3D12_VIDEO_PROCESS_DEINTERLACE_FLAGS DeinterlaceMode; D3D12_VIDEO_PROCESS_FILTER_FLAGS Filters; D3D12_VIDEO_PROCESS_FEATURE_FLAGS FeatureSupport; DXGI_RATIONAL InputFrameRate;
  DXGI_RATIONAL OutputFrameRate; BOOL EnableAutoProcessing; UINT PastFrames; UINT FutureFrames;
} D3D12_FEATURE_DATA_VIDEO_PROCESS_REFERENCE_INFO;
typedef struct D3D12_FEATURE_DATA_VIDEO_ARCHITECTURE { BOOL IOCoherent; } D3D12_FEATURE_DATA_VIDEO_ARCHITECTURE;
typedef struct D3D12_FEATURE_DATA_VIDEO_EXTENSION_COMMAND_COUNT { UINT NodeIndex; UINT CommandCount; } D3D12_FEATURE_DATA_VIDEO_EXTENSION_COMMAND_COUNT;
typedef struct D3D12_FEATURE_DATA_VIDEO_DECODE_PROTECTED_RESOURCES { UINT NodeIndex; D3D12_VIDEO_DECODE_CONFIGURATION Configuration; D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAGS SupportFlags; } D3D12_FEATURE_DATA_VIDEO_DECODE_PROTECTED_RESOURCES;
typedef struct D3D12_FEATURE_DATA_VIDEO_PROCESS_PROTECTED_RESOURCES { UINT NodeIndex; D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAGS SupportFlags; } D3D12_FEATURE_DATA_VIDEO_PROCESS_PROTECTED_RESOURCES;
typedef struct D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES { UINT NodeIndex; D3D12_VIDEO_PROTECTED_RESOURCE_SUPPORT_FLAGS SupportFlags; } D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_PROTECTED_RESOURCES;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC { UINT NodeIndex; D3D12_VIDEO_ENCODER_CODEC Codec; BOOL IsSupported; } D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC;
typedef struct D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS { UINT64 Status; UINT64 NumMacroblocksAffected; DXGI_RATIONAL FrameRate; UINT BitRate; } D3D12_QUERY_DATA_VIDEO_DECODE_STATISTICS;
typedef struct D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 {
  D3D12_VIDEO_PROCESS_INPUT_STREAM InputStream[2]; D3D12_VIDEO_PROCESS_TRANSFORM Transform; D3D12_VIDEO_PROCESS_INPUT_STREAM_FLAGS Flags; D3D12_VIDEO_PROCESS_INPUT_STREAM_RATE RateInfo;
  INT FilterLevels[32]; D3D12_VIDEO_PROCESS_ALPHA_BLENDING AlphaBlending; D3D12_VIDEO_FIELD_TYPE FieldType;
} D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1;

#ifndef __ID3D12VideoDecoder1_INTERFACE_DEFINED__
#define __ID3D12VideoDecoder1_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoDecoder1, 0x79a2e5fb, 0xccd2, 0x469a, 0x9f, 0xde, 0x19, 0x5d, 0x10, 0x95, 0x1f, 0x7e);
typedef struct ID3D12VideoDecoder1 ID3D12VideoDecoder1;
typedef struct ID3D12VideoDecoder1Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoDecoder1 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoDecoder1 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoDecoder1 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoDecoder1 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoDecoder1 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoDecoder1 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoDecoder1 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoDecoder1 *This,
        REFIID riid,
        void **device);

    /*** ID3D12VideoDecoder methods ***/
    D3D12_VIDEO_DECODER_DESC * (STDMETHODCALLTYPE *GetDesc)(
        ID3D12VideoDecoder1 *This,
        D3D12_VIDEO_DECODER_DESC *__ret);

    
    /*** ID3D12VideoDecoder1 methods ***/
    HRESULT (STDMETHODCALLTYPE *GetProtectedResourceSession)(
        ID3D12VideoDecoder1 *This,
        REFIID riid,
        void **ppProtectedSession);

    END_INTERFACE
} ID3D12VideoDecoder1Vtbl;
struct ID3D12VideoDecoder1 {
    CONST_VTBL ID3D12VideoDecoder1Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoDecoderHeap1_INTERFACE_DEFINED__
#define __ID3D12VideoDecoderHeap1_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoDecoderHeap1, 0xda1d98c5, 0x539f, 0x41b2, 0xbf, 0x6b, 0x11, 0x98, 0xa0, 0x3b, 0x6d, 0x26);
typedef struct ID3D12VideoDecoderHeap1 ID3D12VideoDecoderHeap1;
typedef struct ID3D12VideoDecoderHeap1Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoDecoderHeap1 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoDecoderHeap1 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoDecoderHeap1 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoDecoderHeap1 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoDecoderHeap1 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoDecoderHeap1 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoDecoderHeap1 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoDecoderHeap1 *This,
        REFIID riid,
        void **device);

    /*** ID3D12VideoDecoderHeap methods ***/
    D3D12_VIDEO_DECODER_HEAP_DESC * (STDMETHODCALLTYPE *GetDesc)(
        ID3D12VideoDecoderHeap1 *This,
        D3D12_VIDEO_DECODER_HEAP_DESC *__ret);

    
    /*** ID3D12VideoDecoderHeap1 methods ***/
    HRESULT (STDMETHODCALLTYPE *GetProtectedResourceSession)(
        ID3D12VideoDecoderHeap1 *This,
        REFIID riid,
        void **ppProtectedSession);

    END_INTERFACE
} ID3D12VideoDecoderHeap1Vtbl;
struct ID3D12VideoDecoderHeap1 {
    CONST_VTBL ID3D12VideoDecoderHeap1Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoProcessor1_INTERFACE_DEFINED__
#define __ID3D12VideoProcessor1_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoProcessor1, 0xf3cfe615, 0x553f, 0x425c, 0x86, 0xd8, 0xee, 0x8c, 0x1b, 0x1f, 0xb0, 0x1c);
typedef struct ID3D12VideoProcessor1 ID3D12VideoProcessor1;
typedef struct ID3D12VideoProcessor1Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoProcessor1 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoProcessor1 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoProcessor1 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoProcessor1 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoProcessor1 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoProcessor1 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoProcessor1 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoProcessor1 *This,
        REFIID riid,
        void **device);

    /*** ID3D12VideoProcessor methods ***/
    UINT (STDMETHODCALLTYPE *GetNodeMask)(
        ID3D12VideoProcessor1 *This);

    UINT (STDMETHODCALLTYPE *GetNumInputStreamDescs)(
        ID3D12VideoProcessor1 *This);

    HRESULT (STDMETHODCALLTYPE *GetInputStreamDescs)(
        ID3D12VideoProcessor1 *This,
        UINT num_input_stream_descs,
        D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *input_stream_descs);

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC * (STDMETHODCALLTYPE *GetOutputStreamDesc)(
        ID3D12VideoProcessor1 *This,
        D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *__ret);

    
    /*** ID3D12VideoProcessor1 methods ***/
    HRESULT (STDMETHODCALLTYPE *GetProtectedResourceSession)(
        ID3D12VideoProcessor1 *This,
        REFIID riid,
        void **ppProtectedSession);

    END_INTERFACE
} ID3D12VideoProcessor1Vtbl;
struct ID3D12VideoProcessor1 {
    CONST_VTBL ID3D12VideoProcessor1Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoDecodeCommandList2_INTERFACE_DEFINED__
#define __ID3D12VideoDecodeCommandList2_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoDecodeCommandList2, 0x6e120880, 0xc114, 0x4153, 0x80, 0x36, 0xd2, 0x47, 0x05, 0x1e, 0x17, 0x29);
typedef struct ID3D12VideoDecodeCommandList2 ID3D12VideoDecodeCommandList2;
typedef struct ID3D12VideoDecodeCommandList2Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoDecodeCommandList2 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoDecodeCommandList2 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoDecodeCommandList2 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoDecodeCommandList2 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoDecodeCommandList2 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoDecodeCommandList2 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoDecodeCommandList2 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoDecodeCommandList2 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12VideoDecodeCommandList2 *This);

    /*** ID3D12VideoDecodeCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12VideoDecodeCommandList2 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12CommandAllocator *allocator);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12VideoDecodeCommandList2 *This);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12VideoDecodeCommandList2 *This,
        UINT barriers_count,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT queries_count,
        ID3D12Resource *destination_buffer,
        UINT64 aligned_destination_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12VideoDecodeCommandList2 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12VideoDecodeCommandList2 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12VideoDecodeCommandList2 *This);

    void (STDMETHODCALLTYPE *DecodeFrame)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12VideoDecoder *decoder,
        const D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS *output_arguments,
        const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *input_arguments);

    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12VideoDecodeCommandList2 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    /*** ID3D12VideoDecodeCommandList1 methods ***/
    void (STDMETHODCALLTYPE *DecodeFrame1)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12VideoDecoder *decoder,
        const D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS1 *output_arguments,
        const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *input_arguments);

    
    /*** ID3D12VideoDecodeCommandList2 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12ProtectedResourceSession *pProtectedResourceSession);

    /*** ID3D12VideoDecodeCommandList2 methods ***/
    void (STDMETHODCALLTYPE *InitializeExtensionCommand)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pInitializationParameters,
        SIZE_T InitializationParametersSizeInBytes);

    /*** ID3D12VideoDecodeCommandList2 methods ***/
    void (STDMETHODCALLTYPE *ExecuteExtensionCommand)(
        ID3D12VideoDecodeCommandList2 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pExecutionParameters,
        SIZE_T ExecutionParametersSizeInBytes);

    END_INTERFACE
} ID3D12VideoDecodeCommandList2Vtbl;
struct ID3D12VideoDecodeCommandList2 {
    CONST_VTBL ID3D12VideoDecodeCommandList2Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoDecodeCommandList3_INTERFACE_DEFINED__
#define __ID3D12VideoDecodeCommandList3_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoDecodeCommandList3, 0x2aee8c37, 0x9562, 0x42da, 0x8a, 0xbf, 0x61, 0xef, 0xeb, 0x2e, 0x45, 0x13);
typedef struct ID3D12VideoDecodeCommandList3 ID3D12VideoDecodeCommandList3;
typedef struct ID3D12VideoDecodeCommandList3Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoDecodeCommandList3 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoDecodeCommandList3 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoDecodeCommandList3 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoDecodeCommandList3 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoDecodeCommandList3 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoDecodeCommandList3 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoDecodeCommandList3 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoDecodeCommandList3 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12VideoDecodeCommandList3 *This);

    /*** ID3D12VideoDecodeCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12VideoDecodeCommandList3 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12CommandAllocator *allocator);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12VideoDecodeCommandList3 *This);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12VideoDecodeCommandList3 *This,
        UINT barriers_count,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT queries_count,
        ID3D12Resource *destination_buffer,
        UINT64 aligned_destination_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12VideoDecodeCommandList3 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12VideoDecodeCommandList3 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12VideoDecodeCommandList3 *This);

    void (STDMETHODCALLTYPE *DecodeFrame)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12VideoDecoder *decoder,
        const D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS *output_arguments,
        const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *input_arguments);

    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12VideoDecodeCommandList3 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    /*** ID3D12VideoDecodeCommandList1 methods ***/
    void (STDMETHODCALLTYPE *DecodeFrame1)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12VideoDecoder *decoder,
        const D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS1 *output_arguments,
        const D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS *input_arguments);

    
    /*** ID3D12VideoDecodeCommandList2 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12ProtectedResourceSession *pProtectedResourceSession);

    /*** ID3D12VideoDecodeCommandList2 methods ***/
    void (STDMETHODCALLTYPE *InitializeExtensionCommand)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pInitializationParameters,
        SIZE_T InitializationParametersSizeInBytes);

    /*** ID3D12VideoDecodeCommandList2 methods ***/
    void (STDMETHODCALLTYPE *ExecuteExtensionCommand)(
        ID3D12VideoDecodeCommandList3 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pExecutionParameters,
        SIZE_T ExecutionParametersSizeInBytes);

    /*** ID3D12VideoDecodeCommandList3 methods ***/
    void (STDMETHODCALLTYPE *Barrier)(
        ID3D12VideoDecodeCommandList3 *This,
        UINT32 NumBarrierGroups,
        const D3D12_BARRIER_GROUP *pBarrierGroups);

    END_INTERFACE
} ID3D12VideoDecodeCommandList3Vtbl;
struct ID3D12VideoDecodeCommandList3 {
    CONST_VTBL ID3D12VideoDecodeCommandList3Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoProcessCommandList1_INTERFACE_DEFINED__
#define __ID3D12VideoProcessCommandList1_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoProcessCommandList1, 0x542c5c4d, 0x7596, 0x434f, 0x8c, 0x93, 0x4e, 0xfa, 0x67, 0x66, 0xf2, 0x67);
typedef struct ID3D12VideoProcessCommandList1 ID3D12VideoProcessCommandList1;
typedef struct ID3D12VideoProcessCommandList1Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoProcessCommandList1 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoProcessCommandList1 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoProcessCommandList1 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoProcessCommandList1 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoProcessCommandList1 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoProcessCommandList1 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoProcessCommandList1 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoProcessCommandList1 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12VideoProcessCommandList1 *This);

    /*** ID3D12VideoProcessCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12VideoProcessCommandList1 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12CommandAllocator *allocator);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12VideoProcessCommandList1 *This);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12VideoProcessCommandList1 *This,
        UINT num_barriers,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT num_queries,
        ID3D12Resource *destination_buffer,
        UINT64 aligned_destination_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12VideoProcessCommandList1 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12VideoProcessCommandList1 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12VideoProcessCommandList1 *This);

    void (STDMETHODCALLTYPE *ProcessFrames)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12VideoProcessor *video_processor,
        const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *output_arguments,
        UINT num_input_streams,
        const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *input_arguments);

    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12VideoProcessCommandList1 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    
    /*** ID3D12VideoProcessCommandList1 methods ***/
    void (STDMETHODCALLTYPE *ProcessFrames1)(
        ID3D12VideoProcessCommandList1 *This,
        ID3D12VideoProcessor *pVideoProcessor,
        const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *pOutputArguments,
        UINT NumInputStreams,
        const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 *pInputArguments);

    END_INTERFACE
} ID3D12VideoProcessCommandList1Vtbl;
struct ID3D12VideoProcessCommandList1 {
    CONST_VTBL ID3D12VideoProcessCommandList1Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoProcessCommandList2_INTERFACE_DEFINED__
#define __ID3D12VideoProcessCommandList2_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoProcessCommandList2, 0xdb525ae4, 0x6ad6, 0x473c, 0xba, 0xa7, 0x59, 0xb2, 0xe3, 0x70, 0x82, 0xe4);
typedef struct ID3D12VideoProcessCommandList2 ID3D12VideoProcessCommandList2;
typedef struct ID3D12VideoProcessCommandList2Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoProcessCommandList2 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoProcessCommandList2 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoProcessCommandList2 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoProcessCommandList2 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoProcessCommandList2 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoProcessCommandList2 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoProcessCommandList2 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoProcessCommandList2 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12VideoProcessCommandList2 *This);

    /*** ID3D12VideoProcessCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12VideoProcessCommandList2 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12CommandAllocator *allocator);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12VideoProcessCommandList2 *This);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12VideoProcessCommandList2 *This,
        UINT num_barriers,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT num_queries,
        ID3D12Resource *destination_buffer,
        UINT64 aligned_destination_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12VideoProcessCommandList2 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12VideoProcessCommandList2 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12VideoProcessCommandList2 *This);

    void (STDMETHODCALLTYPE *ProcessFrames)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12VideoProcessor *video_processor,
        const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *output_arguments,
        UINT num_input_streams,
        const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *input_arguments);

    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12VideoProcessCommandList2 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    
    /*** ID3D12VideoProcessCommandList1 methods ***/
    void (STDMETHODCALLTYPE *ProcessFrames1)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12VideoProcessor *pVideoProcessor,
        const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *pOutputArguments,
        UINT NumInputStreams,
        const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 *pInputArguments);

    /*** ID3D12VideoProcessCommandList2 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12ProtectedResourceSession *pProtectedResourceSession);

    /*** ID3D12VideoProcessCommandList2 methods ***/
    void (STDMETHODCALLTYPE *InitializeExtensionCommand)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pInitializationParameters,
        SIZE_T InitializationParametersSizeInBytes);

    /*** ID3D12VideoProcessCommandList2 methods ***/
    void (STDMETHODCALLTYPE *ExecuteExtensionCommand)(
        ID3D12VideoProcessCommandList2 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pExecutionParameters,
        SIZE_T ExecutionParametersSizeInBytes);

    END_INTERFACE
} ID3D12VideoProcessCommandList2Vtbl;
struct ID3D12VideoProcessCommandList2 {
    CONST_VTBL ID3D12VideoProcessCommandList2Vtbl* lpVtbl;
};
#endif

#ifndef __ID3D12VideoProcessCommandList3_INTERFACE_DEFINED__
#define __ID3D12VideoProcessCommandList3_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoProcessCommandList3, 0x1a0a4ca4, 0x9f08, 0x40ce, 0x95, 0x58, 0xb4, 0x11, 0xfd, 0x26, 0x66, 0xff);
typedef struct ID3D12VideoProcessCommandList3 ID3D12VideoProcessCommandList3;
typedef struct ID3D12VideoProcessCommandList3Vtbl {
    BEGIN_INTERFACE


    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoProcessCommandList3 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoProcessCommandList3 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoProcessCommandList3 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoProcessCommandList3 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoProcessCommandList3 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoProcessCommandList3 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoProcessCommandList3 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoProcessCommandList3 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12VideoProcessCommandList3 *This);

    /*** ID3D12VideoProcessCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12VideoProcessCommandList3 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12CommandAllocator *allocator);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12VideoProcessCommandList3 *This);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12VideoProcessCommandList3 *This,
        UINT num_barriers,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT num_queries,
        ID3D12Resource *destination_buffer,
        UINT64 aligned_destination_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12VideoProcessCommandList3 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12VideoProcessCommandList3 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12VideoProcessCommandList3 *This);

    void (STDMETHODCALLTYPE *ProcessFrames)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12VideoProcessor *video_processor,
        const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *output_arguments,
        UINT num_input_streams,
        const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *input_arguments);

    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12VideoProcessCommandList3 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    
    /*** ID3D12VideoProcessCommandList1 methods ***/
    void (STDMETHODCALLTYPE *ProcessFrames1)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12VideoProcessor *pVideoProcessor,
        const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *pOutputArguments,
        UINT NumInputStreams,
        const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS1 *pInputArguments);

    /*** ID3D12VideoProcessCommandList2 methods ***/
    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12ProtectedResourceSession *pProtectedResourceSession);

    /*** ID3D12VideoProcessCommandList2 methods ***/
    void (STDMETHODCALLTYPE *InitializeExtensionCommand)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pInitializationParameters,
        SIZE_T InitializationParametersSizeInBytes);

    /*** ID3D12VideoProcessCommandList2 methods ***/
    void (STDMETHODCALLTYPE *ExecuteExtensionCommand)(
        ID3D12VideoProcessCommandList3 *This,
        ID3D12VideoExtensionCommand *pExtensionCommand,
        const void *pExecutionParameters,
        SIZE_T ExecutionParametersSizeInBytes);

    /*** ID3D12VideoProcessCommandList3 methods ***/
    void (STDMETHODCALLTYPE *Barrier)(
        ID3D12VideoProcessCommandList3 *This,
        UINT32 NumBarrierGroups,
        const D3D12_BARRIER_GROUP *pBarrierGroups);

    END_INTERFACE
} ID3D12VideoProcessCommandList3Vtbl;
struct ID3D12VideoProcessCommandList3 {
    CONST_VTBL ID3D12VideoProcessCommandList3Vtbl* lpVtbl;
};
#endif

/* ---- video encoding (what mingw's d3d12video.h lacks of it: the feature structures it leaves out, ID3D12VideoEncodeCommandList3), DirectX-Headers' layouts ---- */
typedef struct D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_SIZE {
  UINT NodeIndex; DXGI_FORMAT InputFormat; D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE BlockSize; D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION Precision; D3D12_VIDEO_SIZE_RANGE SizeRange;
  BOOL Protected; UINT64 MotionVectorHeapMemoryPoolL0Size; UINT64 MotionVectorHeapMemoryPoolL1Size; UINT64 MotionEstimatorMemoryPoolL0Size; UINT64 MotionEstimatorMemoryPoolL1Size;
} D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR_SIZE;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL {
  UINT NodeIndex; D3D12_VIDEO_ENCODER_CODEC Codec; D3D12_VIDEO_ENCODER_PROFILE_DESC Profile; BOOL IsSupported; D3D12_VIDEO_ENCODER_LEVEL_SETTING MinSupportedLevel;
  D3D12_VIDEO_ENCODER_LEVEL_SETTING MaxSupportedLevel;
} D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT { UINT NodeIndex; D3D12_VIDEO_ENCODER_CODEC Codec; UINT ResolutionRatiosCount; } D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT;
typedef struct D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC { UINT WidthRatio; UINT HeightRatio; } D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION {
  UINT NodeIndex; D3D12_VIDEO_ENCODER_CODEC Codec; UINT ResolutionRatiosCount; BOOL IsSupported; D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC MinResolutionSupported;
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC MaxResolutionSupported; UINT ResolutionWidthMultipleRequirement; UINT ResolutionHeightMultipleRequirement;
  D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC *pResolutionRatios;
} D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT {
  UINT NodeIndex; D3D12_VIDEO_ENCODER_CODEC Codec; D3D12_VIDEO_ENCODER_PROFILE_DESC Profile; DXGI_FORMAT Format; BOOL IsSupported;
} D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE {
  UINT NodeIndex; D3D12_VIDEO_ENCODER_CODEC Codec; D3D12_VIDEO_ENCODER_PROFILE_DESC Profile; D3D12_VIDEO_ENCODER_LEVEL_SETTING Level; D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE SubregionMode;
  BOOL IsSupported;
} D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE { D3D12_VIDEO_ENCODER_HEAP_DESC HeapDesc; BOOL IsSupported; UINT64 MemoryPoolL0Size; UINT64 MemoryPoolL1Size; } D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE;
/* (the D3D12_VIDEO_ENCODER_SUPPORT_FLAGS of later SDKs that mingw's enumeration does not name) */
#define D3D12_VIDEO_ENCODER_SUPPORT_FLAG_READABLE_RECONSTRUCTED_PICTURE_LAYOUT_AVAILABLE_ ((D3D12_VIDEO_ENCODER_SUPPORT_FLAGS)0x8000)
/* (and the other flags of later SDKs the H.264 encoder uses: support, validation, heap, picture control and codec configuration support flags) */
#define D3D12_VIDEO_ENCODER_SUPPORT_FLAG_FRAME_PSNR_METADATA_AVAILABLE_ ((D3D12_VIDEO_ENCODER_SUPPORT_FLAGS)0x200000)
#define D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SUBREGIONS_PSNR_METADATA_AVAILABLE_ ((D3D12_VIDEO_ENCODER_SUPPORT_FLAGS)0x400000)
#define D3D12_VIDEO_ENCODER_SUPPORT_FLAG_INTRA_REFRESH_NO_SUBREGION_LAYOUT_CONSTRAINTS_AVAILABLE_ ((D3D12_VIDEO_ENCODER_SUPPORT_FLAGS)0x1000000)
#define D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_DATA_NOT_SUPPORTED_ ((D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)0x1000)
#define D3D12_VIDEO_ENCODER_VALIDATION_FLAG_QPMAP_NOT_SUPPORTED_ ((D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)0x2000)
#define D3D12_VIDEO_ENCODER_VALIDATION_FLAG_DIRTY_REGIONS_NOT_SUPPORTED_ ((D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)0x4000)
#define D3D12_VIDEO_ENCODER_VALIDATION_FLAG_MOTION_SEARCH_NOT_SUPPORTED_ ((D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)0x8000)
#define D3D12_VIDEO_ENCODER_VALIDATION_FLAG_FRAME_ANALYSIS_NOT_SUPPORTED_ ((D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)0x10000)
#define D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_SUBREGION_NOTIFICATION_ARRAY_OF_BUFFERS_ ((D3D12_VIDEO_ENCODER_HEAP_FLAGS)0x1)
#define D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_SUBREGION_NOTIFICATION_SINGLE_BUFFER_ ((D3D12_VIDEO_ENCODER_HEAP_FLAGS)0x2)
#define D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_DIRTY_REGIONS_ ((D3D12_VIDEO_ENCODER_HEAP_FLAGS)0x4)
#define D3D12_VIDEO_ENCODER_HEAP_FLAG_ALLOW_RATE_CONTROL_FRAME_ANALYSIS_ ((D3D12_VIDEO_ENCODER_HEAP_FLAGS)0x8)
#define D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_ENABLE_QUANTIZATION_MATRIX_INPUT_ ((D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS)0x2)
#define D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_ENABLE_DIRTY_REGIONS_INPUT_ ((D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS)0x4)
#define D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_ENABLE_MOTION_VECTORS_INPUT_ ((D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS)0x8)
#define D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE_SUPPORT_ ((D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAGS)0x80)
#define D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_REQUEST_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE_ ((D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAGS)0x2)

#ifndef __ID3D12VideoEncodeCommandList3_INTERFACE_DEFINED__
#define __ID3D12VideoEncodeCommandList3_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D12VideoEncodeCommandList3, 0x7f027b22, 0x1515, 0x4e85, 0xaa, 0x0d, 0x02, 0x64, 0x86, 0x58, 0x05, 0x76);
typedef struct ID3D12VideoEncodeCommandList3 ID3D12VideoEncodeCommandList3;
typedef struct ID3D12VideoEncodeCommandList3Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D12VideoEncodeCommandList3 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D12VideoEncodeCommandList3 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D12VideoEncodeCommandList3 *This);

    /*** ID3D12Object methods ***/
    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D12VideoEncodeCommandList3 *This,
        REFGUID guid,
        UINT *data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D12VideoEncodeCommandList3 *This,
        REFGUID guid,
        UINT data_size,
        const void *data);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D12VideoEncodeCommandList3 *This,
        REFGUID guid,
        const IUnknown *data);

    HRESULT (STDMETHODCALLTYPE *SetName)(
        ID3D12VideoEncodeCommandList3 *This,
        const WCHAR *name);

    /*** ID3D12DeviceChild methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDevice)(
        ID3D12VideoEncodeCommandList3 *This,
        REFIID riid,
        void **device);

    /*** ID3D12CommandList methods ***/
    D3D12_COMMAND_LIST_TYPE (STDMETHODCALLTYPE *GetType)(
        ID3D12VideoEncodeCommandList3 *This);

    /*** ID3D12VideoEncodeCommandList methods ***/
    HRESULT (STDMETHODCALLTYPE *Close)(
        ID3D12VideoEncodeCommandList3 *This);

    HRESULT (STDMETHODCALLTYPE *Reset)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12CommandAllocator *allocator);

    void (STDMETHODCALLTYPE *ClearState)(
        ID3D12VideoEncodeCommandList3 *This);

    void (STDMETHODCALLTYPE *ResourceBarrier)(
        ID3D12VideoEncodeCommandList3 *This,
        UINT barriers_count,
        const D3D12_RESOURCE_BARRIER *barriers);

    void (STDMETHODCALLTYPE *DiscardResource)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12Resource *resource,
        const D3D12_DISCARD_REGION *region);

    void (STDMETHODCALLTYPE *BeginQuery)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *EndQuery)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT index);

    void (STDMETHODCALLTYPE *ResolveQueryData)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12QueryHeap *query_heap,
        D3D12_QUERY_TYPE type,
        UINT start_index,
        UINT queries_count,
        ID3D12Resource *destination_buffer,
        UINT64 aligned_destination_buffer_offset);

    void (STDMETHODCALLTYPE *SetPredication)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12Resource *buffer,
        UINT64 aligned_buffer_offset,
        D3D12_PREDICATION_OP operation);

    void (STDMETHODCALLTYPE *SetMarker)(
        ID3D12VideoEncodeCommandList3 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *BeginEvent)(
        ID3D12VideoEncodeCommandList3 *This,
        UINT metadata,
        const void *data,
        UINT size);

    void (STDMETHODCALLTYPE *EndEvent)(
        ID3D12VideoEncodeCommandList3 *This);

    void (STDMETHODCALLTYPE *EstimateMotion)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12VideoMotionEstimator *motion_estimator,
        const D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT *output_arguments,
        const D3D12_VIDEO_MOTION_ESTIMATOR_INPUT *input_arguments);

    void (STDMETHODCALLTYPE *ResolveMotionVectorHeap)(
        ID3D12VideoEncodeCommandList3 *This,
        const D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT *output_arguments,
        const D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT *input_arguments);

    void (STDMETHODCALLTYPE *WriteBufferImmediate)(
        ID3D12VideoEncodeCommandList3 *This,
        UINT count,
        const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes);

    void (STDMETHODCALLTYPE *SetProtectedResourceSession)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12ProtectedResourceSession *protected_resource_session);

    /*** ID3D12VideoEncodeCommandList1 methods ***/
    void (STDMETHODCALLTYPE *InitializeExtensionCommand)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12VideoExtensionCommand *extension_command,
        const void *initialization_parameters,
        SIZE_T initialization_parameters_size_in_bytes);

    void (STDMETHODCALLTYPE *ExecuteExtensionCommand)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12VideoExtensionCommand *extension_command,
        const void *execution_parameters,
        SIZE_T execution_parameters_size_in_bytes);

    /*** ID3D12VideoEncodeCommandList3 methods ***/
    void (STDMETHODCALLTYPE *EncodeFrame)(
        ID3D12VideoEncodeCommandList3 *This,
        ID3D12VideoEncoder *encoder,
        ID3D12VideoEncoderHeap *heap,
        const D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS *input_arguments,
        const D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS *output_arguments);

    void (STDMETHODCALLTYPE *ResolveEncoderOutputMetadata)(
        ID3D12VideoEncodeCommandList3 *This,
        const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS *input_arguments,
        const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS *output_arguments);

    /*** ID3D12VideoEncodeCommandList3 methods ***/
    void (STDMETHODCALLTYPE *Barrier)(
        ID3D12VideoEncodeCommandList3 *This,
        UINT32 NumBarrierGroups,
        const D3D12_BARRIER_GROUP *pBarrierGroups);

    END_INTERFACE
} ID3D12VideoEncodeCommandList3Vtbl;
struct ID3D12VideoEncodeCommandList3 {
    CONST_VTBL ID3D12VideoEncodeCommandList3Vtbl* lpVtbl;
};
#endif

/* ---- video encoding of the later SDKs (DirectX-Headers 1.6xx): ID3D12VideoDevice4, ID3D12VideoEncoderHeap1, ID3D12VideoEncodeCommandList4 - the input maps (QP, dirty
 * regions, motion hints) and their resolve, the optional metadata, the SUPPORT2 / HEAP_SIZE1 / RESOURCE_REQUIREMENTS1 / QPMAP_INPUT / DIRTY_REGIONS / MOTION_SEARCH /
 * RATE_CONTROL_FRAME_ANALYSIS queries ---- */
#ifndef __ID3D12VideoEncodeCommandList4_INTERFACE_DEFINED__
#define __ID3D12VideoEncodeCommandList4_INTERFACE_DEFINED__
#define D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1 ((D3D12_FEATURE_VIDEO)48)
#define D3D12_FEATURE_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT ((D3D12_FEATURE_VIDEO)49)
#define D3D12_FEATURE_VIDEO_ENCODER_QPMAP_INPUT ((D3D12_FEATURE_VIDEO)50)
#define D3D12_FEATURE_VIDEO_ENCODER_DIRTY_REGIONS ((D3D12_FEATURE_VIDEO)51)
#define D3D12_FEATURE_VIDEO_ENCODER_MOTION_SEARCH ((D3D12_FEATURE_VIDEO)52)
#define D3D12_FEATURE_VIDEO_ENCODER_SUPPORT2 ((D3D12_FEATURE_VIDEO)55)
#define D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE1 ((D3D12_FEATURE_VIDEO)56)
#define D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS ((D3D12_FEATURE_VIDEO)57)
DEFINE_GUID(IID_ID3D12VideoEncoderHeap1, 0xea8f1968, 0x4aa0, 0x43a4, 0x9d, 0x30, 0xba, 0x86, 0xec, 0x84, 0xd4, 0xf9);
DEFINE_GUID(IID_ID3D12VideoDevice4, 0xe59ad09e, 0xf1ae, 0x42bb, 0x89, 0x83, 0x9f, 0x6e, 0x55, 0x86, 0xc4, 0xeb);
DEFINE_GUID(IID_ID3D12VideoEncodeCommandList4, 0x69aeb5b7, 0x55f2, 0x4012, 0x8b, 0x73, 0x3a, 0x88, 0xd6, 0x5a, 0x20, 0x4c);
typedef struct D3D12_VIDEO_ENCODER_HEAP_DESC1
    {
    UINT NodeMask;
    D3D12_VIDEO_ENCODER_HEAP_FLAGS Flags;
    D3D12_VIDEO_ENCODER_CODEC EncodeCodec;
    D3D12_VIDEO_ENCODER_PROFILE_DESC EncodeProfile;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING EncodeLevel;
    UINT ResolutionsListCount;
      const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *pResolutionList;
    UINT Pow2DownscaleFactor;
    } 	D3D12_VIDEO_ENCODER_HEAP_DESC1;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE1
    {
    D3D12_VIDEO_ENCODER_HEAP_DESC1 HeapDesc;
    BOOL IsSupported;
    UINT64 MemoryPoolL0Size;
    UINT64 MemoryPoolL1Size;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE1;
typedef
enum D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS
    {
        D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_NONE	= 0,
        D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_QP_MAP	= 0x1,
        D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_SATD_MAP	= 0x2,
        D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_RC_BIT_ALLOCATION_MAP	= 0x4,
        D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_FRAME_PSNR	= 0x8,
        D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_SUBREGIONS_PSNR	= 0x10
    } 	D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS(D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS)
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_CODEC Codec;
    D3D12_VIDEO_ENCODER_PROFILE_DESC Profile;
    DXGI_FORMAT InputFormat;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC PictureTargetResolution;
    BOOL IsSupported;
    UINT CompressedBitstreamBufferAccessAlignment;
    UINT EncoderMetadataBufferAccessAlignment;
    UINT MaxEncoderOutputMetadataBufferSize;
    D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS OptionalMetadata;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION CodecConfiguration;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC EncoderOutputMetadataQPMapTextureDimensions;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC EncoderOutputMetadataSATDMapTextureDimensions;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC EncoderOutputMetadataBitAllocationMapTextureDimensions;
    UINT EncoderOutputMetadataFramePSNRComponentsNumber;
    UINT EncoderOutputMetadataSubregionsPSNRComponentsNumber;
    UINT EncoderOutputMetadataSubregionsPSNRResolvedMetadataBufferSize;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1;
typedef struct D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_PSNR_RESOLVED_LAYOUT
    {
    float PSNRY;
    float PSNRU;
    float PSNRV;
    } 	D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_PSNR_RESOLVED_LAYOUT;
typedef
enum D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE
    {
        D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER	= 0,
        D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_GPU_TEXTURE	= 1
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE;
typedef
enum D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE
    {
        D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE_DIRTY	= 0,
        D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE_SKIP	= 1
    } 	D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE;
typedef struct D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO
    {
    D3D12_VIDEO_ENCODER_CODEC Codec;
    D3D12_VIDEO_ENCODER_PROFILE_DESC Profile;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING Level;
    DXGI_FORMAT InputFormat;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC InputResolution;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION CodecConfiguration;
    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE SubregionFrameEncoding;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA SubregionFrameEncodingData;
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO;
typedef
enum D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE
    {
        D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE_QUANTIZATION_MATRIX	= 0,
        D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE_DIRTY_REGIONS	= 1,
        D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE_MOTION_VECTORS	= 2
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE;
typedef
enum D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE
    {
        D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_FULL_SEARCH	= 0,
        D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_START_HINT	= 1,
        D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_START_HINT_LIMITED_DISTANCE	= 2
    } 	D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_QPMAP_INPUT
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO SessionInfo;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    BOOL IsSupported;
    UINT MapSourcePreferenceRanking;
    UINT BlockSize;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_QPMAP_INPUT;
typedef
enum D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAGS
    {
        D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_NONE	= 0,
        D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_REPEAT_FRAME	= 0x1,
        D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_DIRTY_REGIONS	= 0x2,
        D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_DIRTY_REGIONS_REQUIRE_FULL_ROW	= 0x4
    } 	D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS(D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAGS)
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_DIRTY_REGIONS
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO SessionInfo;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE MapValuesType;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAGS SupportFlags;
    UINT MapSourcePreferenceRanking;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_DIRTY_REGIONS;
typedef
enum D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION
    {
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_FULL_PIXEL	= 0,
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_HALF_PIXEL	= 1,
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_QUARTER_PIXEL	= 2
    } 	D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION;
typedef
enum D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAGS
    {
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAG_NONE	= 0,
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAG_FULL_PIXEL	= ( 1 << D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_FULL_PIXEL ) ,
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAG_HALF_PIXEL	= ( 1 << D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_HALF_PIXEL ) ,
        D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAG_QUARTER_PIXEL	= ( 1 << D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_QUARTER_PIXEL )
    } 	D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS(D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAGS)
typedef
enum D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAGS
    {
        D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_NONE	= 0,
        D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_SUPPORTED	= 0x1,
        D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_MULTIPLE_HINTS	= 0x2,
        D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_GPU_TEXTURE_MULTIPLE_REFERENCES	= 0x4
    } 	D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS(D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAGS)
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_MOTION_SEARCH
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO SessionInfo;
    D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE MotionSearchMode;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    BOOL BidirectionalRefFrameEnabled;
    D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAGS SupportFlags;
    UINT MaxMotionHints;
    UINT MinDeviation;
    UINT MaxDeviation;
    UINT MapSourcePreferenceRanking;
    D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAGS MotionUnitPrecisionSupport;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_MOTION_SEARCH;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO SessionInfo;
    D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE MapType;
    BOOL IsSupported;
    UINT64 MaxResolvedBufferAllocationSize;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT;
typedef struct D3D12_VIDEO_ENCODER_QPMAP_CONFIGURATION
    {
    BOOL Enabled;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    } 	D3D12_VIDEO_ENCODER_QPMAP_CONFIGURATION;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_QPMAP
    {
    UINT MapSourcePreferenceRanking;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_QPMAP;
typedef struct D3D12_VIDEO_ENCODER_DIRTY_REGIONS_CONFIGURATION
    {
    BOOL Enabled;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE MapValuesType;
    } 	D3D12_VIDEO_ENCODER_DIRTY_REGIONS_CONFIGURATION;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_DIRTY_REGIONS
    {
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAGS DirtyRegionsSupportFlags;
    UINT MapSourcePreferenceRanking;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_DIRTY_REGIONS;
typedef struct D3D12_VIDEO_ENCODER_MOTION_SEARCH_CONFIGURATION
    {
    BOOL Enabled;
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE MotionSearchMode;
    BOOL BidirectionalRefFrameEnabled;
    } 	D3D12_VIDEO_ENCODER_MOTION_SEARCH_CONFIGURATION;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_MOTION_SEARCH
    {
    UINT MaxMotionHints;
    UINT MinDeviation;
    UINT MaxDeviation;
    UINT MapSourcePreferenceRanking;
    D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAGS MotionUnitPrecisionSupportFlags;
    D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAGS SupportFlags;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_MOTION_SEARCH;
typedef
enum D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAGS
    {
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_NONE	= 0,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_INTRACODED_FRAME_SUPPORTED	= 0x1,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_UNIDIR_INTER_FRAME_SUPPORTED	= 0x2,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_BIDIR_INTER_FRAME_SUPPORTED	= 0x4,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_EXTERNAL_DPB_DOWNSCALING	= 0x8,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_DYNAMIC_1ST_PASS_SKIP	= 0x10,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_DYNAMIC_DOWNSCALE_FACTOR_CHANGE_KEY_FRAME	= 0x20,
        D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_SUPPORTED	= ( ( D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_INTRACODED_FRAME_SUPPORTED | D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_UNIDIR_INTER_FRAME_SUPPORTED )  | D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAG_BIDIR_INTER_FRAME_SUPPORTED )
    } 	D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS(D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAGS)
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_CODEC Codec;
    D3D12_VIDEO_ENCODER_PROFILE_DESC Profile;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING Level;
    DXGI_FORMAT InputFormat;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC InputResolution;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION CodecConfiguration;
    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE SubregionFrameEncoding;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA SubregionFrameEncodingData;
    D3D12_VIDEO_ENCODER_QPMAP_CONFIGURATION QPMap;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_CONFIGURATION DirtyRegions;
    D3D12_VIDEO_ENCODER_MOTION_SEARCH_CONFIGURATION MotionSearch;
    UINT Pow2DownscaleFactor;
    D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAGS SupportFlags;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_FRAME_ANALYSIS
    {
    D3D12_VIDEO_ENCODER_RATE_CONTROL_FRAME_ANALYSIS_SUPPORT_FLAGS SupportFlags;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_FRAME_ANALYSIS;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS1
    {
    UINT MaxSubregionsNumber;
    UINT MaxIntraRefreshFrameDuration;
    UINT SubregionBlockPixelsSize;
    UINT QPMapRegionPixelsSize;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_QPMAP QPMap;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_DIRTY_REGIONS DirtyRegions;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_MOTION_SEARCH MotionSearch;
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_FRAME_ANALYSIS FrameAnalysis;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS1;
typedef struct D3D12_VIDEO_ENCODER_FRAME_ANALYSIS_CONFIGURATION
    {
    BOOL Enabled;
    UINT Pow2DownscaleFactor;
    } 	D3D12_VIDEO_ENCODER_FRAME_ANALYSIS_CONFIGURATION;
typedef struct D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2
    {
    UINT NodeIndex;
    D3D12_VIDEO_ENCODER_CODEC Codec;
    DXGI_FORMAT InputFormat;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION CodecConfiguration;
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE CodecGopSequence;
    D3D12_VIDEO_ENCODER_RATE_CONTROL RateControl;
    D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE IntraRefresh;
    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE SubregionFrameEncoding;
    UINT ResolutionsListCount;
      const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *pResolutionList;
    UINT MaxReferenceFramesInDPB;
    D3D12_VIDEO_ENCODER_VALIDATION_FLAGS ValidationFlags;
    D3D12_VIDEO_ENCODER_SUPPORT_FLAGS SupportFlags;
    D3D12_VIDEO_ENCODER_PROFILE_DESC SuggestedProfile;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING SuggestedLevel;
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS1 *pResolutionDependentSupport;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA SubregionFrameEncodingData;
    UINT MaxQualityVsSpeed;
    D3D12_VIDEO_ENCODER_QPMAP_CONFIGURATION QPMap;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_CONFIGURATION DirtyRegions;
    D3D12_VIDEO_ENCODER_MOTION_SEARCH_CONFIGURATION MotionSearch;
    D3D12_VIDEO_ENCODER_FRAME_ANALYSIS_CONFIGURATION FrameAnalysis;
    } 	D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2;
typedef struct D3D12_VIDEO_ENCODER_DIRTY_RECT_INFO
    {
    BOOL FullFrameIdentical;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE MapValuesType;
    UINT NumDirtyRects;
      RECT *pDirtyRects;
    UINT SourceDPBFrameReference;
    } 	D3D12_VIDEO_ENCODER_DIRTY_RECT_INFO;
typedef struct D3D12_VIDEO_ENCODER_DIRTY_REGIONS
    {
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    union
        {
        ID3D12Resource *pOpaqueLayoutBuffer;
        D3D12_VIDEO_ENCODER_DIRTY_RECT_INFO *pCPUBuffer;
        } 	;
    } 	D3D12_VIDEO_ENCODER_DIRTY_REGIONS;
typedef struct D3D12_VIDEO_ENCODER_QUANTIZATION_OPAQUE_MAP
    {
    ID3D12Resource *pOpaqueQuantizationMap;
    } 	D3D12_VIDEO_ENCODER_QUANTIZATION_OPAQUE_MAP;
typedef struct D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_CONFIG
    {
    D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE MotionSearchMode;
    UINT SearchDeviationLimit;
    } 	D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_CONFIG;
typedef struct D3D12_VIDEO_ENCODER_MOVE_RECT
    {
    POINT SourcePoint;
    RECT DestRect;
    } 	D3D12_VIDEO_ENCODER_MOVE_RECT;
typedef
enum D3D12_VIDEO_ENCODER_MOVEREGION_INFO_FLAGS
    {
        D3D12_VIDEO_ENCODER_MOVEREGION_INFO_FLAG_NONE	= 0,
        D3D12_VIDEO_ENCODER_MOVEREGION_INFO_FLAG_MULTIPLE_HINTS	= 0x1
    } 	D3D12_VIDEO_ENCODER_MOVEREGION_INFO_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS(D3D12_VIDEO_ENCODER_MOVEREGION_INFO_FLAGS)
typedef struct D3D12_VIDEO_ENCODER_MOVEREGION_INFO
    {
    UINT NumMoveRegions;
      D3D12_VIDEO_ENCODER_MOVE_RECT *pMoveRegions;
    D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_CONFIG MotionSearchModeConfiguration;
    UINT SourceDPBFrameReference;
    D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION MotionUnitPrecision;
    D3D12_VIDEO_ENCODER_MOVEREGION_INFO_FLAGS Flags;
    } 	D3D12_VIDEO_ENCODER_MOVEREGION_INFO;
typedef struct D3D12_VIDEO_ENCODER_FRAME_MOTION_VECTORS
    {
    D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE MapSource;
    union
        {
        ID3D12Resource *pOpaqueLayoutBuffer;
        D3D12_VIDEO_ENCODER_MOVEREGION_INFO *pCPUBuffer;
        } 	;
    } 	D3D12_VIDEO_ENCODER_FRAME_MOTION_VECTORS;
typedef struct D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA1
    {
    UINT DataSize;
    union
        {
        const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 *pH264PicData;
        const struct D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC2 *pHEVCPicData;   /* (only the H.264 picture data is read here) */
        const struct D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA *pAV1PicData;
        } 	;
    } 	D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA1;
typedef struct D3D12_VIDEO_ENCODER_FRAME_ANALYSIS
    {
    ID3D12Resource *pDownscaledFrame;
    UINT64 Subresource;
    D3D12_VIDEO_ENCODE_REFERENCE_FRAMES DownscaledReferences;
    } 	D3D12_VIDEO_ENCODER_FRAME_ANALYSIS;
typedef struct D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC1
    {
    UINT IntraRefreshFrameIndex;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS Flags;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA1 PictureControlCodecData;
    D3D12_VIDEO_ENCODE_REFERENCE_FRAMES ReferenceFrames;
    D3D12_VIDEO_ENCODER_FRAME_MOTION_VECTORS MotionVectors;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS DirtyRects;
    D3D12_VIDEO_ENCODER_QUANTIZATION_OPAQUE_MAP QuantizationTextureMap;
    D3D12_VIDEO_ENCODER_FRAME_ANALYSIS FrameAnalysis;
    } 	D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC1;
typedef struct D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS1
    {
    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC SequenceControlDesc;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC1 PictureControlDesc;
    ID3D12Resource *pInputFrame;
    UINT InputFrameSubresource;
    UINT CurrentFrameBitstreamMetadataSize;
    D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS OptionalMetadata;
    } 	D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS1;
typedef
enum D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM_BUFFER_MODE
    {
        D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM_BUFFER_MODE_ARRAY_OF_BUFFERS	= 0,
        D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM_BUFFER_MODE_SINGLE_BUFFER	= 1
    } 	D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM_BUFFER_MODE;
typedef struct D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM
    {
    D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM_BUFFER_MODE BufferMode;
    UINT ExpectedSubregionCount;
      UINT64 *pSubregionBitstreamsBaseOffsets;
      ID3D12Resource **ppSubregionBitstreams;
      ID3D12Resource **ppSubregionSizes;
      ID3D12Resource **ppSubregionOffsets;
      ID3D12Fence **ppSubregionFences;
      UINT64 *pSubregionFenceValues;
    } 	D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM;
typedef
enum D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM_NOTIFICATION_MODE
    {
        D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM_NOTIFICATION_MODE_FULL_FRAME	= 0,
        D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM_NOTIFICATION_MODE_SUBREGIONS	= 1
    } 	D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM_NOTIFICATION_MODE;
typedef struct D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM1
    {
    D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM_NOTIFICATION_MODE NotificationMode;
    union
        {
        D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM FrameOutputBuffer;
        D3D12_VIDEO_ENCODER_SUBREGION_COMPRESSED_BITSTREAM SubregionOutputBuffers;
        } 	;
    } 	D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM1;
typedef struct D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS1
    {
    D3D12_VIDEO_ENCODER_COMPRESSED_BITSTREAM1 Bitstream;
    D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE ReconstructedPicture;
    D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER EncoderOutputMetadata;
    D3D12_VIDEO_ENCODER_RECONSTRUCTED_PICTURE FrameAnalysisReconstructedPicture;
    } 	D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS1;
typedef struct D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS1
    {
    D3D12_VIDEO_ENCODER_CODEC EncoderCodec;
    D3D12_VIDEO_ENCODER_PROFILE_DESC EncoderProfile;
    DXGI_FORMAT EncoderInputFormat;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC EncodedPictureEffectiveResolution;
    D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER HWLayoutMetadata;
    D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS OptionalMetadata;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION CodecConfiguration;
    } 	D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS1;
typedef struct D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS1
    {
    D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER ResolvedLayoutMetadata;
    ID3D12Resource *pOutputQPMap;
    ID3D12Resource *pOutputSATDMap;
    ID3D12Resource *pOutputBitAllocationMap;
    D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER ResolvedFramePSNRData;
    D3D12_VIDEO_ENCODER_ENCODE_OPERATION_METADATA_BUFFER ResolvedSubregionsPSNRData;
    } 	D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS1;
typedef struct D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_QUANTIZATION_MATRIX
    {
    ID3D12Resource *pQuantizationMap;
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_QUANTIZATION_MATRIX;
typedef struct D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_DIRTY_REGIONS
    {
    BOOL FullFrameIdentical;
    D3D12_VIDEO_ENCODER_DIRTY_REGIONS_MAP_VALUES_MODE MapValuesType;
    ID3D12Resource *pDirtyRegionsMap;
    UINT SourceDPBFrameReference;
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_DIRTY_REGIONS;
typedef struct D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_MOTION_VECTORS
    {
    D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE_CONFIG MotionSearchModeConfiguration;
    UINT NumHintsPerPixel;
      ID3D12Resource **ppMotionVectorMaps;
      UINT *pMotionVectorMapsSubresources;
      ID3D12Resource **ppMotionVectorMapsMetadata;
      UINT *pMotionVectorMapsMetadataSubresources;
    D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION MotionUnitPrecision;
    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA1 PictureControlConfiguration;
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_MOTION_VECTORS;
typedef struct D3D12_VIDEO_ENCODER_INPUT_MAP_DATA
    {
    D3D12_VIDEO_ENCODER_INPUT_MAP_TYPE MapType;
    union
        {
        D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_QUANTIZATION_MATRIX Quantization;
        D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_DIRTY_REGIONS DirtyRegions;
        D3D12_VIDEO_ENCODER_INPUT_MAP_DATA_MOTION_VECTORS MotionVectors;
        } 	;
    } 	D3D12_VIDEO_ENCODER_INPUT_MAP_DATA;
typedef struct D3D12_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT_INPUT_ARGUMENTS
    {
    D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO SessionInfo;
    D3D12_VIDEO_ENCODER_INPUT_MAP_DATA InputData;
    } 	D3D12_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT_INPUT_ARGUMENTS;
typedef struct D3D12_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT_OUTPUT_ARGUMENTS
    {
    ID3D12Resource *pOpaqueLayoutBuffer;
    } 	D3D12_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT_OUTPUT_ARGUMENTS;
typedef struct ID3D12VideoEncoderHeap1 ID3D12VideoEncoderHeap1;
typedef struct ID3D12VideoEncoderHeap1Vtbl {
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )(
            ID3D12VideoEncoderHeap1 * This,
            REFIID riid,
              void **ppvObject);
        ULONG ( STDMETHODCALLTYPE *AddRef )(
            ID3D12VideoEncoderHeap1 * This);
        ULONG ( STDMETHODCALLTYPE *Release )(
            ID3D12VideoEncoderHeap1 * This);
        HRESULT ( STDMETHODCALLTYPE *GetPrivateData )(
            ID3D12VideoEncoderHeap1 * This,
              REFGUID guid,
              UINT *pDataSize,
              void *pData);
        HRESULT ( STDMETHODCALLTYPE *SetPrivateData )(
            ID3D12VideoEncoderHeap1 * This,
              REFGUID guid,
              UINT DataSize,
              const void *pData);
        HRESULT ( STDMETHODCALLTYPE *SetPrivateDataInterface )(
            ID3D12VideoEncoderHeap1 * This,
              REFGUID guid,
              const IUnknown *pData);
        HRESULT ( STDMETHODCALLTYPE *SetName )(
            ID3D12VideoEncoderHeap1 * This,
              LPCWSTR Name);
        HRESULT ( STDMETHODCALLTYPE *GetDevice )(
            ID3D12VideoEncoderHeap1 * This,
            REFIID riid,
              void **ppvDevice);
        UINT ( STDMETHODCALLTYPE *GetNodeMask )(
            ID3D12VideoEncoderHeap1 * This);
        D3D12_VIDEO_ENCODER_HEAP_FLAGS ( STDMETHODCALLTYPE *GetEncoderHeapFlags )(
            ID3D12VideoEncoderHeap1 * This);
        D3D12_VIDEO_ENCODER_CODEC ( STDMETHODCALLTYPE *GetCodec )(
            ID3D12VideoEncoderHeap1 * This);
        HRESULT ( STDMETHODCALLTYPE *GetCodecProfile )(
            ID3D12VideoEncoderHeap1 * This,
              D3D12_VIDEO_ENCODER_PROFILE_DESC dstProfile);
        HRESULT ( STDMETHODCALLTYPE *GetCodecLevel )(
            ID3D12VideoEncoderHeap1 * This,
              D3D12_VIDEO_ENCODER_LEVEL_SETTING dstLevel);
        UINT ( STDMETHODCALLTYPE *GetResolutionListCount )(
            ID3D12VideoEncoderHeap1 * This);
        HRESULT ( STDMETHODCALLTYPE *GetResolutionList )(
            ID3D12VideoEncoderHeap1 * This,
            const UINT ResolutionsListCount,
              D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *pResolutionList);
        UINT ( STDMETHODCALLTYPE *GetPow2DownscaleFactor )(
            ID3D12VideoEncoderHeap1 * This);
    } ID3D12VideoEncoderHeap1Vtbl;
struct ID3D12VideoEncoderHeap1 {
    CONST_VTBL struct ID3D12VideoEncoderHeap1Vtbl *lpVtbl;
};
typedef struct ID3D12VideoDevice4 ID3D12VideoDevice4;
typedef struct ID3D12VideoDevice4Vtbl {
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )(
            ID3D12VideoDevice4 * This,
            REFIID riid,
              void **ppvObject);
        ULONG ( STDMETHODCALLTYPE *AddRef )(
            ID3D12VideoDevice4 * This);
        ULONG ( STDMETHODCALLTYPE *Release )(
            ID3D12VideoDevice4 * This);
        HRESULT ( STDMETHODCALLTYPE *CheckFeatureSupport )(
            ID3D12VideoDevice4 * This,
            D3D12_FEATURE_VIDEO FeatureVideo,
            void *pFeatureSupportData,
            UINT FeatureSupportDataSize);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoDecoder )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_DECODER_DESC *pDesc,
              REFIID riid,
              void **ppVideoDecoder);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoDecoderHeap )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_DECODER_HEAP_DESC *pVideoDecoderHeapDesc,
              REFIID riid,
              void **ppVideoDecoderHeap);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoProcessor )(
            ID3D12VideoDevice4 * This,
            UINT NodeMask,
              const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *pOutputStreamDesc,
            UINT NumInputStreamDescs,
              const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *pInputStreamDescs,
              REFIID riid,
              void **ppVideoProcessor);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoMotionEstimator )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_MOTION_ESTIMATOR_DESC *pDesc,
              ID3D12ProtectedResourceSession *pProtectedResourceSession,
              REFIID riid,
              void **ppVideoMotionEstimator);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoMotionVectorHeap )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC *pDesc,
              ID3D12ProtectedResourceSession *pProtectedResourceSession,
              REFIID riid,
              void **ppVideoMotionVectorHeap);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoDecoder1 )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_DECODER_DESC *pDesc,
              ID3D12ProtectedResourceSession *pProtectedResourceSession,
              REFIID riid,
              void **ppVideoDecoder);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoDecoderHeap1 )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_DECODER_HEAP_DESC *pVideoDecoderHeapDesc,
              ID3D12ProtectedResourceSession *pProtectedResourceSession,
              REFIID riid,
              void **ppVideoDecoderHeap);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoProcessor1 )(
            ID3D12VideoDevice4 * This,
            UINT NodeMask,
              const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *pOutputStreamDesc,
            UINT NumInputStreamDescs,
              const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *pInputStreamDescs,
              ID3D12ProtectedResourceSession *pProtectedResourceSession,
              REFIID riid,
              void **ppVideoProcessor);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoExtensionCommand )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_EXTENSION_COMMAND_DESC *pDesc,
              const void *pCreationParameters,
            SIZE_T CreationParametersDataSizeInBytes,
              ID3D12ProtectedResourceSession *pProtectedResourceSession,
              REFIID riid,
              void **ppVideoExtensionCommand);
        HRESULT ( STDMETHODCALLTYPE *ExecuteExtensionCommand )(
            ID3D12VideoDevice4 * This,
              ID3D12VideoExtensionCommand *pExtensionCommand,
              const void *pExecutionParameters,
            SIZE_T ExecutionParametersSizeInBytes,
              void *pOutputData,
            SIZE_T OutputDataSizeInBytes);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoEncoder )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_ENCODER_DESC *pDesc,
              REFIID riid,
              void **ppVideoEncoder);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoEncoderHeap )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_ENCODER_HEAP_DESC *pDesc,
              REFIID riid,
              void **ppVideoEncoderHeap);
        HRESULT ( STDMETHODCALLTYPE *CreateVideoEncoderHeap1 )(
            ID3D12VideoDevice4 * This,
              const D3D12_VIDEO_ENCODER_HEAP_DESC1 *pDesc,
              REFIID riid,
              void **ppVideoEncoderHeap);
    } ID3D12VideoDevice4Vtbl;
struct ID3D12VideoDevice4 {
    CONST_VTBL struct ID3D12VideoDevice4Vtbl *lpVtbl;
};
typedef struct ID3D12VideoEncodeCommandList4 ID3D12VideoEncodeCommandList4;
typedef struct ID3D12VideoEncodeCommandList4Vtbl {
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )(
            ID3D12VideoEncodeCommandList4 * This,
            REFIID riid,
              void **ppvObject);
        ULONG ( STDMETHODCALLTYPE *AddRef )(
            ID3D12VideoEncodeCommandList4 * This);
        ULONG ( STDMETHODCALLTYPE *Release )(
            ID3D12VideoEncodeCommandList4 * This);
        HRESULT ( STDMETHODCALLTYPE *GetPrivateData )(
            ID3D12VideoEncodeCommandList4 * This,
              REFGUID guid,
              UINT *pDataSize,
              void *pData);
        HRESULT ( STDMETHODCALLTYPE *SetPrivateData )(
            ID3D12VideoEncodeCommandList4 * This,
              REFGUID guid,
              UINT DataSize,
              const void *pData);
        HRESULT ( STDMETHODCALLTYPE *SetPrivateDataInterface )(
            ID3D12VideoEncodeCommandList4 * This,
              REFGUID guid,
              const IUnknown *pData);
        HRESULT ( STDMETHODCALLTYPE *SetName )(
            ID3D12VideoEncodeCommandList4 * This,
              LPCWSTR Name);
        HRESULT ( STDMETHODCALLTYPE *GetDevice )(
            ID3D12VideoEncodeCommandList4 * This,
            REFIID riid,
              void **ppvDevice);
        D3D12_COMMAND_LIST_TYPE ( STDMETHODCALLTYPE *GetType )(
            ID3D12VideoEncodeCommandList4 * This);
        HRESULT ( STDMETHODCALLTYPE *Close )(
            ID3D12VideoEncodeCommandList4 * This);
        HRESULT ( STDMETHODCALLTYPE *Reset )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12CommandAllocator *pAllocator);
        void ( STDMETHODCALLTYPE *ClearState )(
            ID3D12VideoEncodeCommandList4 * This);
        void ( STDMETHODCALLTYPE *ResourceBarrier )(
            ID3D12VideoEncodeCommandList4 * This,
              UINT NumBarriers,
              const D3D12_RESOURCE_BARRIER *pBarriers);
        void ( STDMETHODCALLTYPE *DiscardResource )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12Resource *pResource,
              const D3D12_DISCARD_REGION *pRegion);
        void ( STDMETHODCALLTYPE *BeginQuery )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12QueryHeap *pQueryHeap,
              D3D12_QUERY_TYPE Type,
              UINT Index);
        void ( STDMETHODCALLTYPE *EndQuery )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12QueryHeap *pQueryHeap,
              D3D12_QUERY_TYPE Type,
              UINT Index);
        void ( STDMETHODCALLTYPE *ResolveQueryData )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12QueryHeap *pQueryHeap,
              D3D12_QUERY_TYPE Type,
              UINT StartIndex,
              UINT NumQueries,
              ID3D12Resource *pDestinationBuffer,
              UINT64 AlignedDestinationBufferOffset);
        void ( STDMETHODCALLTYPE *SetPredication )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12Resource *pBuffer,
              UINT64 AlignedBufferOffset,
              D3D12_PREDICATION_OP Operation);
        void ( STDMETHODCALLTYPE *SetMarker )(
            ID3D12VideoEncodeCommandList4 * This,
            UINT Metadata,
              const void *pData,
            UINT Size);
        void ( STDMETHODCALLTYPE *BeginEvent )(
            ID3D12VideoEncodeCommandList4 * This,
            UINT Metadata,
              const void *pData,
            UINT Size);
        void ( STDMETHODCALLTYPE *EndEvent )(
            ID3D12VideoEncodeCommandList4 * This);
        void ( STDMETHODCALLTYPE *EstimateMotion )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12VideoMotionEstimator *pMotionEstimator,
              const D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT *pOutputArguments,
              const D3D12_VIDEO_MOTION_ESTIMATOR_INPUT *pInputArguments);
        void ( STDMETHODCALLTYPE *ResolveMotionVectorHeap )(
            ID3D12VideoEncodeCommandList4 * This,
            const D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT *pOutputArguments,
            const D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT *pInputArguments);
        void ( STDMETHODCALLTYPE *WriteBufferImmediate )(
            ID3D12VideoEncodeCommandList4 * This,
            UINT Count,
              const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams,
              const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes);
        void ( STDMETHODCALLTYPE *SetProtectedResourceSession )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12ProtectedResourceSession *pProtectedResourceSession);
        void ( STDMETHODCALLTYPE *InitializeExtensionCommand )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12VideoExtensionCommand *pExtensionCommand,
              const void *pInitializationParameters,
            SIZE_T InitializationParametersSizeInBytes);
        void ( STDMETHODCALLTYPE *ExecuteExtensionCommand )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12VideoExtensionCommand *pExtensionCommand,
              const void *pExecutionParameters,
            SIZE_T ExecutionParametersSizeInBytes);
        void ( STDMETHODCALLTYPE *EncodeFrame )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12VideoEncoder *pEncoder,
              ID3D12VideoEncoderHeap *pHeap,
              const D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS *pInputArguments,
              const D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS *pOutputArguments);
        void ( STDMETHODCALLTYPE *ResolveEncoderOutputMetadata )(
            ID3D12VideoEncodeCommandList4 * This,
              const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS *pInputArguments,
              const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS *pOutputArguments);
        void ( STDMETHODCALLTYPE *Barrier )(
            ID3D12VideoEncodeCommandList4 * This,
            UINT32 NumBarrierGroups,
              const D3D12_BARRIER_GROUP *pBarrierGroups);
        void ( STDMETHODCALLTYPE *EncodeFrame1 )(
            ID3D12VideoEncodeCommandList4 * This,
              ID3D12VideoEncoder *pEncoder,
              ID3D12VideoEncoderHeap1 *pHeap,
              const D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS1 *pInputArguments,
              const D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS1 *pOutputArguments);
        void ( STDMETHODCALLTYPE *ResolveEncoderOutputMetadata1 )(
            ID3D12VideoEncodeCommandList4 * This,
              const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS1 *pInputArguments,
              const D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS1 *pOutputArguments);
        void ( STDMETHODCALLTYPE *ResolveInputParamLayout )(
            ID3D12VideoEncodeCommandList4 * This,
              const D3D12_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT_INPUT_ARGUMENTS *pInputArguments,
              const D3D12_VIDEO_ENCODER_RESOLVE_INPUT_PARAM_LAYOUT_OUTPUT_ARGUMENTS *pOutputArguments);
    } ID3D12VideoEncodeCommandList4Vtbl;
struct ID3D12VideoEncodeCommandList4 {
    CONST_VTBL struct ID3D12VideoEncodeCommandList4Vtbl *lpVtbl;
};
#endif
