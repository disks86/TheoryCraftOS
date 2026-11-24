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
typedef enum D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAGS { D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAG_NONE = 0 } D3D12_VIDEO_DECODE_HISTOGRAM_COMPONENT_FLAGS;
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
