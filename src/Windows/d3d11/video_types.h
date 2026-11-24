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

/* What mingw's d3d11_4.h lacks of the Windows SDK's (10.0.16299 and later): ID3D11VideoDevice2, ID3D11VideoContext3 and their types, as the SDK has them (the IIDs,
 * the order of the methods, the structures and enumerations). scripts/gen-dx-vtables.py reads this file beside the mingw headers. */
#pragma once

#ifndef __ID3D11VideoDevice2_FWD_DEFINED__
#define __ID3D11VideoDevice2_FWD_DEFINED__
typedef interface ID3D11VideoDevice2 ID3D11VideoDevice2;
#endif
#ifndef __ID3D11VideoContext3_FWD_DEFINED__
#define __ID3D11VideoContext3_FWD_DEFINED__
typedef interface ID3D11VideoContext3 ID3D11VideoContext3;
#endif

typedef enum D3D11_FEATURE_VIDEO { D3D11_FEATURE_VIDEO_DECODER_HISTOGRAM = 0 } D3D11_FEATURE_VIDEO;
typedef enum D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT {
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_Y = 0, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_U = 1, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_V = 2,
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_R = 0, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_G = 1, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_B = 2,
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_A = 3
} D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT;
typedef enum D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAGS {
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_NONE = 0, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_Y = 1, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_U = 2,
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_V = 4, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_R = 1, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_G = 2,
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_B = 4, D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAG_A = 8
} D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAGS;
typedef struct D3D11_FEATURE_DATA_VIDEO_DECODER_HISTOGRAM {
  D3D11_VIDEO_DECODER_DESC DecoderDesc;
  D3D11_VIDEO_DECODER_HISTOGRAM_COMPONENT_FLAGS Components;
  UINT BinCount;
  UINT CounterBitDepth;
} D3D11_FEATURE_DATA_VIDEO_DECODER_HISTOGRAM;
typedef enum D3D11_CRYPTO_SESSION_KEY_EXCHANGE_FLAGS { D3D11_CRYPTO_SESSION_KEY_EXCHANGE_FLAG_NONE = 0 } D3D11_CRYPTO_SESSION_KEY_EXCHANGE_FLAGS;
typedef struct D3D11_VIDEO_DECODER_BUFFER_DESC2 {
  D3D11_VIDEO_DECODER_BUFFER_TYPE BufferType;
  UINT DataOffset;
  UINT DataSize;
  void *pIV;
  UINT IVSize;
  D3D11_VIDEO_DECODER_SUB_SAMPLE_MAPPING_BLOCK *pSubSampleMappingBlock;
  UINT SubSampleMappingCount;
  UINT cBlocksStripeEncrypted;
  UINT cBlocksStripeClear;
} D3D11_VIDEO_DECODER_BUFFER_DESC2;

#ifndef __ID3D11VideoDevice2_INTERFACE_DEFINED__
#define __ID3D11VideoDevice2_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D11VideoDevice2, 0x59c0cb01, 0x35f0, 0x4a70, 0x8f,0x67, 0x87,0x90,0x5c,0x90,0x6a,0x53);
typedef struct ID3D11VideoDevice2Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D11VideoDevice2 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D11VideoDevice2 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D11VideoDevice2 *This);

    /*** ID3D11VideoDevice methods ***/
    HRESULT (STDMETHODCALLTYPE *CreateVideoDecoder)(
        ID3D11VideoDevice2 *This,
        const D3D11_VIDEO_DECODER_DESC *pVideoDesc,
        const D3D11_VIDEO_DECODER_CONFIG *pConfig,
        ID3D11VideoDecoder **ppDecoder);

    HRESULT (STDMETHODCALLTYPE *CreateVideoProcessor)(
        ID3D11VideoDevice2 *This,
        ID3D11VideoProcessorEnumerator *pEnum,
        UINT RateConversionIndex,
        ID3D11VideoProcessor **ppVideoProcessor);

    HRESULT (STDMETHODCALLTYPE *CreateAuthenticatedChannel)(
        ID3D11VideoDevice2 *This,
        D3D11_AUTHENTICATED_CHANNEL_TYPE ChannelType,
        ID3D11AuthenticatedChannel **ppAuthenticatedChannel);

    HRESULT (STDMETHODCALLTYPE *CreateCryptoSession)(
        ID3D11VideoDevice2 *This,
        const GUID *pCryptoType,
        const GUID *pDecoderProfile,
        const GUID *pKeyExchangeType,
        ID3D11CryptoSession **ppCryptoSession);

    HRESULT (STDMETHODCALLTYPE *CreateVideoDecoderOutputView)(
        ID3D11VideoDevice2 *This,
        ID3D11Resource *pResource,
        const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC *pDesc,
        ID3D11VideoDecoderOutputView **ppVDOVView);

    HRESULT (STDMETHODCALLTYPE *CreateVideoProcessorInputView)(
        ID3D11VideoDevice2 *This,
        ID3D11Resource *pResource,
        ID3D11VideoProcessorEnumerator *pEnum,
        const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC *pDesc,
        ID3D11VideoProcessorInputView **ppVPIView);

    HRESULT (STDMETHODCALLTYPE *CreateVideoProcessorOutputView)(
        ID3D11VideoDevice2 *This,
        ID3D11Resource *pResource,
        ID3D11VideoProcessorEnumerator *pEnum,
        const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC *pDesc,
        ID3D11VideoProcessorOutputView **ppVPOView);

    HRESULT (STDMETHODCALLTYPE *CreateVideoProcessorEnumerator)(
        ID3D11VideoDevice2 *This,
        const D3D11_VIDEO_PROCESSOR_CONTENT_DESC *pDesc,
        ID3D11VideoProcessorEnumerator **ppEnum);

    UINT (STDMETHODCALLTYPE *GetVideoDecoderProfileCount)(
        ID3D11VideoDevice2 *This);

    HRESULT (STDMETHODCALLTYPE *GetVideoDecoderProfile)(
        ID3D11VideoDevice2 *This,
        UINT Index,
        GUID *pDecoderProfile);

    HRESULT (STDMETHODCALLTYPE *CheckVideoDecoderFormat)(
        ID3D11VideoDevice2 *This,
        const GUID *pDecoderProfile,
        DXGI_FORMAT Format,
        WINBOOL *pSupported);

    HRESULT (STDMETHODCALLTYPE *GetVideoDecoderConfigCount)(
        ID3D11VideoDevice2 *This,
        const D3D11_VIDEO_DECODER_DESC *pDesc,
        UINT *pCount);

    HRESULT (STDMETHODCALLTYPE *GetVideoDecoderConfig)(
        ID3D11VideoDevice2 *This,
        const D3D11_VIDEO_DECODER_DESC *pDesc,
        UINT Index,
        D3D11_VIDEO_DECODER_CONFIG *pConfig);

    HRESULT (STDMETHODCALLTYPE *GetContentProtectionCaps)(
        ID3D11VideoDevice2 *This,
        const GUID *pCryptoType,
        const GUID *pDecoderProfile,
        D3D11_VIDEO_CONTENT_PROTECTION_CAPS *pCaps);

    HRESULT (STDMETHODCALLTYPE *CheckCryptoKeyExchange)(
        ID3D11VideoDevice2 *This,
        const GUID *pCryptoType,
        const GUID *pDecoderProfile,
        UINT Index,
        GUID *pKeyExchangeType);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D11VideoDevice2 *This,
        REFGUID guid,
        UINT DataSize,
        const void *pData);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D11VideoDevice2 *This,
        REFGUID guid,
        const IUnknown *pData);

    /*** ID3D11VideoDevice1 methods ***/
    HRESULT (STDMETHODCALLTYPE *GetCryptoSessionPrivateDataSize)(
        ID3D11VideoDevice2 *This,
        const GUID *crypto_type,
        const GUID *decoder_profile,
        const GUID *key_exchange_type,
        UINT *input_size,
        UINT *output_size);

    HRESULT (STDMETHODCALLTYPE *GetVideoDecoderCaps)(
        ID3D11VideoDevice2 *This,
        const GUID *decoder_profile,
        UINT sample_width,
        UINT sample_height,
        const DXGI_RATIONAL *framerate,
        UINT bitrate,
        const GUID *crypto_type,
        UINT *decoder_caps);

    HRESULT (STDMETHODCALLTYPE *CheckVideoDecoderDownsampling)(
        ID3D11VideoDevice2 *This,
        const D3D11_VIDEO_DECODER_DESC *input_desc,
        DXGI_COLOR_SPACE_TYPE input_colour_space,
        const D3D11_VIDEO_DECODER_CONFIG *input_config,
        const DXGI_RATIONAL *framerate,
        const D3D11_VIDEO_SAMPLE_DESC *output_desc,
        WINBOOL *supported,
        WINBOOL *real_time_hint);

    HRESULT (STDMETHODCALLTYPE *RecommendVideoDecoderDownsampleParameters)(
        ID3D11VideoDevice2 *This,
        const D3D11_VIDEO_DECODER_DESC *input_desc,
        DXGI_COLOR_SPACE_TYPE input_colour_space,
        const D3D11_VIDEO_DECODER_CONFIG *input_config,
        const DXGI_RATIONAL *framerate,
        D3D11_VIDEO_SAMPLE_DESC *recommended_output_desc);


    /*** ID3D11VideoDevice2 methods ***/
    HRESULT (STDMETHODCALLTYPE *CheckFeatureSupport)(
        ID3D11VideoDevice2 *This,
        D3D11_FEATURE_VIDEO Feature,
        void *pFeatureSupportData,
        UINT FeatureSupportDataSize);

    HRESULT (STDMETHODCALLTYPE *NegotiateCryptoSessionKeyExchangeMT)(
        ID3D11VideoDevice2 *This,
        ID3D11CryptoSession *pCryptoSession,
        D3D11_CRYPTO_SESSION_KEY_EXCHANGE_FLAGS flags,
        UINT DataSize,
        void *pData);

    END_INTERFACE
} ID3D11VideoDevice2Vtbl;
interface ID3D11VideoDevice2 { CONST_VTBL ID3D11VideoDevice2Vtbl* lpVtbl; };
#endif

#ifndef __ID3D11VideoContext3_INTERFACE_DEFINED__
#define __ID3D11VideoContext3_INTERFACE_DEFINED__
DEFINE_GUID(IID_ID3D11VideoContext3, 0xa9e2faa0, 0xcb39, 0x418f, 0xa0,0xb7, 0xd8,0xaa,0xd4,0xde,0x67,0x2e);
typedef struct ID3D11VideoContext3Vtbl {
    BEGIN_INTERFACE

    /*** IUnknown methods ***/
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        ID3D11VideoContext3 *This,
        REFIID riid,
        void **ppvObject);

    ULONG (STDMETHODCALLTYPE *AddRef)(
        ID3D11VideoContext3 *This);

    ULONG (STDMETHODCALLTYPE *Release)(
        ID3D11VideoContext3 *This);

    /*** ID3D11DeviceChild methods ***/
    void (STDMETHODCALLTYPE *GetDevice)(
        ID3D11VideoContext3 *This,
        ID3D11Device **ppDevice);

    HRESULT (STDMETHODCALLTYPE *GetPrivateData)(
        ID3D11VideoContext3 *This,
        REFGUID guid,
        UINT *pDataSize,
        void *pData);

    HRESULT (STDMETHODCALLTYPE *SetPrivateData)(
        ID3D11VideoContext3 *This,
        REFGUID guid,
        UINT DataSize,
        const void *pData);

    HRESULT (STDMETHODCALLTYPE *SetPrivateDataInterface)(
        ID3D11VideoContext3 *This,
        REFGUID guid,
        const IUnknown *pData);

    /*** ID3D11VideoContext methods ***/
    HRESULT (STDMETHODCALLTYPE *GetDecoderBuffer)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        D3D11_VIDEO_DECODER_BUFFER_TYPE type,
        UINT *buffer_size,
        void **buffer);

    HRESULT (STDMETHODCALLTYPE *ReleaseDecoderBuffer)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        D3D11_VIDEO_DECODER_BUFFER_TYPE type);

    HRESULT (STDMETHODCALLTYPE *DecoderBeginFrame)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        ID3D11VideoDecoderOutputView *view,
        UINT key_size,
        const void *key);

    HRESULT (STDMETHODCALLTYPE *DecoderEndFrame)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder);

    HRESULT (STDMETHODCALLTYPE *SubmitDecoderBuffers)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        UINT buffers_count,
        const D3D11_VIDEO_DECODER_BUFFER_DESC *buffer_desc);

    HRESULT (STDMETHODCALLTYPE *DecoderExtension)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        const D3D11_VIDEO_DECODER_EXTENSION *extension);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputTargetRect)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL enable,
        const RECT *rect);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputBackgroundColor)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL y_cb_cr,
        const D3D11_VIDEO_COLOR *color);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputColorSpace)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputAlphaFillMode)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE alpha_fill_mode,
        UINT stream_idx);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputConstriction)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL enable,
        SIZE size);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputStereoMode)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL enable);

    HRESULT (STDMETHODCALLTYPE *VideoProcessorSetOutputExtension)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        const GUID *guid,
        UINT data_size,
        void *data);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputTargetRect)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL *enabled,
        RECT *rect);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputBackgroundColor)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL *y_cb_cr,
        D3D11_VIDEO_COLOR *color);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputColorSpace)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputAlphaFillMode)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE *alpha_fill_mode,
        UINT *stream_idx);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputConstriction)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL *enabled,
        SIZE *size);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputStereoMode)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL *enabled);

    HRESULT (STDMETHODCALLTYPE *VideoProcessorGetOutputExtension)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        const GUID *guid,
        UINT data_size,
        void *data);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamFrameFormat)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_FRAME_FORMAT format);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamColorSpace)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamOutputRate)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_PROCESSOR_OUTPUT_RATE rate,
        WINBOOL repeat,
        const DXGI_RATIONAL *custom_rate);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamSourceRect)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        const RECT *rect);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamDestRect)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        const RECT *rect);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamAlpha)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        float alpha);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamPalette)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        UINT entry_count,
        const UINT *entries);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamPixelAspectRatio)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        const DXGI_RATIONAL *src_aspect_ratio,
        const DXGI_RATIONAL *dst_aspect_ratio);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamLumaKey)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        float lower,
        float upper);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamStereoFormat)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        D3D11_VIDEO_PROCESSOR_STEREO_FORMAT format,
        WINBOOL left_view_frame0,
        WINBOOL base_view_frame0,
        D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip_mode,
        int mono_offset);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamAutoProcessingMode)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamFilter)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_PROCESSOR_FILTER filter,
        WINBOOL enable,
        int level);

    HRESULT (STDMETHODCALLTYPE *VideoProcessorSetStreamExtension)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        const GUID *guid,
        UINT data_size,
        void *data);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamFrameFormat)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_FRAME_FORMAT *format);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamColorSpace)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamOutputRate)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_PROCESSOR_OUTPUT_RATE *rate,
        WINBOOL *repeat,
        DXGI_RATIONAL *custom_rate);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamSourceRect)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled,
        RECT *rect);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamDestRect)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled,
        RECT *rect);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamAlpha)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled,
        float *alpha);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamPalette)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        UINT entry_count,
        UINT *entries);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamPixelAspectRatio)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled,
        DXGI_RATIONAL *src_aspect_ratio,
        DXGI_RATIONAL *dst_aspect_ratio);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamLumaKey)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled,
        float *lower,
        float *upper);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamStereoFormat)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled,
        D3D11_VIDEO_PROCESSOR_STEREO_FORMAT *format,
        WINBOOL *left_view_frame0,
        WINBOOL *base_view_frame0,
        D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE *flip_mode,
        int *mono_offset);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamAutoProcessingMode)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enabled);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamFilter)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        D3D11_VIDEO_PROCESSOR_FILTER filter,
        WINBOOL *enabled,
        int *level);

    HRESULT (STDMETHODCALLTYPE *VideoProcessorGetStreamExtension)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        const GUID *guid,
        UINT data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *VideoProcessorBlt)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        ID3D11VideoProcessorOutputView *view,
        UINT frame_idx,
        UINT stream_count,
        const D3D11_VIDEO_PROCESSOR_STREAM *streams);

    HRESULT (STDMETHODCALLTYPE *NegotiateCryptoSessionKeyExchange)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        UINT data_size,
        void *data);

    void (STDMETHODCALLTYPE *EncryptionBlt)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        ID3D11Texture2D *src_surface,
        ID3D11Texture2D *dst_surface,
        UINT iv_size,
        void *iv);

    void (STDMETHODCALLTYPE *DecryptionBlt)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        ID3D11Texture2D *src_surface,
        ID3D11Texture2D *dst_surface,
        D3D11_ENCRYPTED_BLOCK_INFO *block_info,
        UINT key_size,
        const void *key,
        UINT iv_size,
        void *iv);

    void (STDMETHODCALLTYPE *StartSessionKeyRefresh)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        UINT random_number_size,
        void *random_number);

    void (STDMETHODCALLTYPE *FinishSessionKeyRefresh)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session);

    HRESULT (STDMETHODCALLTYPE *GetEncryptionBltKey)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        UINT key_size,
        void *key);

    HRESULT (STDMETHODCALLTYPE *NegotiateAuthenticatedChannelKeyExchange)(
        ID3D11VideoContext3 *This,
        ID3D11AuthenticatedChannel *channel,
        UINT data_size,
        void *data);

    HRESULT (STDMETHODCALLTYPE *QueryAuthenticatedChannel)(
        ID3D11VideoContext3 *This,
        ID3D11AuthenticatedChannel *channel,
        UINT input_size,
        const void *input,
        UINT output_size,
        void *output);

    HRESULT (STDMETHODCALLTYPE *ConfigureAuthenticatedChannel)(
        ID3D11VideoContext3 *This,
        ID3D11AuthenticatedChannel *channel,
        UINT input_size,
        const void *input,
        D3D11_AUTHENTICATED_CONFIGURE_OUTPUT *output);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamRotation)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL enable,
        D3D11_VIDEO_PROCESSOR_ROTATION rotation);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamRotation)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_idx,
        WINBOOL *enable,
        D3D11_VIDEO_PROCESSOR_ROTATION *rotation);

    /*** ID3D11VideoContext1 methods ***/
    HRESULT (STDMETHODCALLTYPE *SubmitDecoderBuffers1)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        UINT buffer_count,
        const D3D11_VIDEO_DECODER_BUFFER_DESC1 *buffer_desc);

    HRESULT (STDMETHODCALLTYPE *GetDataForNewHardwareKey)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        UINT input_size,
        const void *input_data,
        UINT64 *output_data);

    HRESULT (STDMETHODCALLTYPE *CheckCryptoSessionStatus)(
        ID3D11VideoContext3 *This,
        ID3D11CryptoSession *session,
        D3D11_CRYPTO_SESSION_STATUS *status);

    HRESULT (STDMETHODCALLTYPE *DecoderEnableDownsampling)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        DXGI_COLOR_SPACE_TYPE colour_space,
        const D3D11_VIDEO_SAMPLE_DESC *output_desc,
        UINT reference_frame_count);

    HRESULT (STDMETHODCALLTYPE *DecoderUpdateDownsampling)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *decoder,
        const D3D11_VIDEO_SAMPLE_DESC *output_desc);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputColorSpace1)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        DXGI_COLOR_SPACE_TYPE colour_space);

    void (STDMETHODCALLTYPE *VideoProcessorSetOutputShaderUsage)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL shader_usage);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputColorSpace1)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        DXGI_COLOR_SPACE_TYPE *colour_space);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputShaderUsage)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        WINBOOL *shader_usage);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamColorSpace1)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_index,
        DXGI_COLOR_SPACE_TYPE colour_space);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamMirror)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_index,
        WINBOOL enable,
        WINBOOL flip_horizontal,
        WINBOOL flip_vertical);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamColorSpace1)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_index,
        DXGI_COLOR_SPACE_TYPE *colour_space);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamMirror)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_index,
        WINBOOL *enable,
        WINBOOL *flip_horizontal,
        WINBOOL *flip_vertical);

    HRESULT (STDMETHODCALLTYPE *VideoProcessorGetBehaviorHints)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT output_width,
        UINT output_height,
        DXGI_FORMAT output_format,
        UINT stream_count,
        const D3D11_VIDEO_PROCESSOR_STREAM_BEHAVIOR_HINT *streams,
        UINT *behaviour_hints);

    /*** ID3D11VideoContext2 methods ***/
    void (STDMETHODCALLTYPE *VideoProcessorSetOutputHDRMetaData)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        DXGI_HDR_METADATA_TYPE type,
        UINT size,
        const void *meta_data);

    void (STDMETHODCALLTYPE *VideoProcessorGetOutputHDRMetaData)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        DXGI_HDR_METADATA_TYPE *type,
        UINT size,
        void *meta_data);

    void (STDMETHODCALLTYPE *VideoProcessorSetStreamHDRMetaData)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_index,
        DXGI_HDR_METADATA_TYPE type,
        UINT size,
        const void *meta_data);

    void (STDMETHODCALLTYPE *VideoProcessorGetStreamHDRMetaData)(
        ID3D11VideoContext3 *This,
        ID3D11VideoProcessor *processor,
        UINT stream_index,
        DXGI_HDR_METADATA_TYPE *type,
        UINT size,
        void *meta_data);


    /*** ID3D11VideoContext3 methods ***/
    HRESULT (STDMETHODCALLTYPE *DecoderBeginFrame1)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *pDecoder,
        ID3D11VideoDecoderOutputView *pView,
        UINT ContentKeySize,
        const void *pContentKey,
        UINT NumComponentHistograms,
        const UINT *pHistogramOffsets,
        ID3D11Buffer *const *ppHistogramBuffers);

    HRESULT (STDMETHODCALLTYPE *SubmitDecoderBuffers2)(
        ID3D11VideoContext3 *This,
        ID3D11VideoDecoder *pDecoder,
        UINT NumBuffers,
        const D3D11_VIDEO_DECODER_BUFFER_DESC2 *pBufferDesc);

    END_INTERFACE
} ID3D11VideoContext3Vtbl;
interface ID3D11VideoContext3 { CONST_VTBL ID3D11VideoContext3Vtbl* lpVtbl; };
#endif
