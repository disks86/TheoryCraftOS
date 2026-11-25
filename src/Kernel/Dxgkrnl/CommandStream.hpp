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

// The command stream a user-mode driver hands to Kernel::render(): the "DMA buffer" of this kernel. It is not hardware-specific - the kernel validates it against the
// allocation list and the pipelines it names and replays it on the adapter's graphics::IGraphicsDevice - so one user-mode driver serves every adapter.
//
// Layout: a sequence of packets, little-endian 32-bit words. Word 0 of a packet is op | (size_in_words << 16) (the size counts that word); the payload follows.
// Allocations are named by an index into the allocation list passed with the stream (never by address: the kernel decides where memory is), pipelines by their handle.
// State (pass, pipeline, buffers, textures) does not carry over from one stream to the next.
//
//   BeginPass        color_index, depth_index (kNoAllocation: none), flags, clear r, g, b, a (floats), clear depth (float), clear stencil, [up to three more colour indices (kFeatureMultipleTargets)]
//                    [with kPassLayers in the flags, last: for each colour target and then the depth buffer its first layer and its layer count - 2 * (targets + 1) words (the adapter's
//                    kFeature2LayeredTargets: targets that are arrays, volumes or cubes; a primitive goes to the layer its SV_RenderTargetArrayIndex names, the first when that is past
//                    the target's count; a 2D image's is layer 0 of 1, and the depth pair of a pass without one is 0, 1)]
//                    [with kPassLevels in the flags, after those: the level of each colour target and then of the depth buffer - targets + 1 words (the adapter's kFeature2MipmappedLayers: the pass
//                    draws into that level of each, all of one size, the pass's; the layers above are the level's layers - a volume's level has fewer slices -; 0 for the depth of a pass without one)]
//   EndPass
//   BindPipeline     pipeline handle
//   BindVertexBuffer allocation_index, offset
//   BindIndexBuffer  allocation_index, index_type (0: 16 bit, 1: 32 bit), offset
//   PushConstants    byte_count (a multiple of 16, at most the adapter's max_push_constant_bytes: 128 for most, 4096 for the CPU renderer and virgl), then the bytes padded to a whole word
//   BindTexture      unit, allocation_index, filter (filter_word() of DxgTypes.hpp: the filter, a magnification filter of its own - kFeature2SeparateFilters -, the minimum or maximum
//                    reduction - kFeature2SamplerReductions -, coordinates in texels - kFeature2UnnormalizedCoordinates), address_u, address_v (address_word() of DxgTypes.hpp: the
//                    mode, and the border colour in address_u), mip_filter, lod_bias, min_lod, max_lod (floats)
//                    [, view: texture_view_word() of DxgTypes.hpp - the comparison of SampleCmp / GatherCmp, and the stencil plane of a D24S8 depth buffer (kFeature2DepthTextures:
//                    a kAllocDepthStencil | kAllocTexture allocation)]
//                    [, base_level, level_count, first_layer, layer_count]   (after the view word - the 11th to 14th words -: a view, the adapter's kFeature2MipmappedLayers: the unit shows
//                    those levels and layers - a count of 0: to the last -, a volume all its slices; its kind is the one the shader declares: graphics::texture_view_fits says which an
//                    image may have - an array's slice as a 2D texture, six layers of an array as a cube, a cube or a cube array as an array, ...; a pass may sample levels and layers of
//                    its targets it does not draw into)
//                    [, max_anisotropy (1 .. 16), custom border (0 or 1), the border colour's r, g, b, a (bits)]   (after the view - the 15th to 20th words, so a packet that has them has
//                    the view word and the view too, 0 0 0 0 for all of the image -: the adapter's kFeature2SamplerState, graphics::SamplerDesc's anisotropy and border colour of any
//                    value; without the part the sampler has neither). A packet has 9, 10, 14 or 20 words.
//   SetViewport      x, y, width, height, min_depth, max_depth (floats) [, the same for viewports 1 ..]   (more than one - up to graphics::kMaxViewports - needs the adapter's kFeature2Viewports:
//                    a primitive is drawn with the viewport and scissor rectangle its SV_ViewportArrayIndex names, the first when that is past the ones set)
//   SetScissor       x, y, width, height (x and y signed) [, the same for rectangles 1 ..]
//   Draw             vertex_count, first_vertex
//   DrawIndexed      index_count, first_index, vertex_offset (signed)
//   SetTopology      topology (Topology, below) [, strip cut (StripCut: the index that ends a strip or fan; without the word the index buffer's largest index)]
//   CopyRegion       dst_index, dst_x, dst_y, src_index, src_x, src_y, width, height   (colour images of one format, or of two whose pixels - or blocks - are as many bytes; also staging memory)
//                    [, dst_level, dst_layer, src_level, src_layer, layer_count]   (between subresources: the rectangle of layer_count layers - a volume's slices - from those of the
//                    levels; levels other than 0 of a layered image, and block-compressed images - whole blocks, as CopyBufferImage -, need the adapter's kFeature2MipmappedLayers)
//                    [, planes]   (bit 0 the depth, bit 1 the stencil, of a copy between depth buffers; 0: every plane the buffers have)
//                    (depth buffers and multisampled images - images whose contents the system has no memory of - are copied by the device, the adapter's kFeature2DepthCopies: whole
//                    subresources (the rectangle all of a level that is the same size in both, at 0, 0) between two of one format and sample count, the planes of a depth buffer
//                    asked for)
//   GenerateMipmaps  allocation_index   (every level from level 0 by the box filter; with kFeature2MipmappedLayers also of an array's or a cube's layers, a volume - 2 x 2 x 2 - and the wide formats; not
//                    of block-compressed or integer textures)
//   BeginQuery       allocation_index, offset [, kind]   (kind, bits 0-7: a graphics::QueryKind, 0 occlusion count if left out, 1 occlusion predicate, 2 IA vertices, 3 IA primitives, 4 VS invocations,
//                    5 PS invocations, 6 timestamp (kFeatureTimestampQueries; ns, read when the query ends), 7 / 8 stream output primitives written / needed (both kFeatureStatisticsQueries and
//                    kFeatureStreamOutput; bits 8-9 of the kind word say which output stream), 9 timestamp frequency, 10 timestamp disjoint (kFeatureTimestampQueries), 11 pipeline statistics
//                    (D3D11_QUERY_DATA_PIPELINE_STATISTICS / D3D12_QUERY_DATA_PIPELINE_STATISTICS: eleven 64-bit values, graphics::PipelineStatistic), 12 kQueryKindPipelineStatistics1
//                    (D3D12_QUERY_DATA_PIPELINE_STATISTICS1: the eleven and the amplification and mesh shader invocations and mesh primitives, 0: there are no such stages), 13
//                    kQueryKindSoStatistics (D3D12_QUERY_DATA_SO_STATISTICS of the output stream in bits 8-9: the primitives written, then those needed; like 7 and 8); the others need
//                    kFeatureStatisticsQueries. Bit 16, kQueryAccumulate: the counts are added to what the place holds (a predicate or the disjoint flag: 1 if either is not 0) instead of
//                    replacing it - a query that goes on from an earlier stream (not for 6 and 9). Every query needs the adapter's kFeatureOcclusionQueries; the allocation is a
//                    kAllocQueryResult buffer and `offset` a multiple of 8 with room for the result (8 bytes a value). Any number of queries may be open at once, of one kind or of several,
//                    nested or overlapping, inside passes and outside: each counts the draws and dispatches of every pass between its BeginQuery and its EndQuery (a clear, copy, resolve or
//                    mipmap counts nothing). Two open queries do not write the same bytes, and every query begun is ended in the stream. A stream's queries take at most
//                    kMaxOcclusionQueries device queries: kind 13 takes two, the others one)
//   SetStreamOutput  slot, allocation_index (kNoAllocation: none), offset[, end]   (the buffer the pipeline's stream output declaration puts buffer `slot` in: a kAllocStreamOutput buffer, offset a multiple of 4 or kStreamOutputAppendOffset;
//                    `end`, optional: the byte where its writes stop - a primitive that does not fit before it is not written, nor any after it, as at the buffer's end (Direct3D 12's SizeInBytes);
//                    kStreamOutputNoEnd or left out: the buffer's end. Anywhere, the adapter's kFeatureStreamOutput)
//   SetRootSignature root signature handle   (Direct3D 12 binding: forgets the root arguments; the draws and dispatches that follow bind textures, storage buffers and constants from them: see D3d12.hpp)
//   SetDescriptorHeaps count (1 or 2), heap handles   (at most one of each type: views and samplers)
//   SetRootConstants parameter, first value, count, values   (32-bit values into a root constants parameter)
//   SetRootDescriptor parameter, allocation_index, offset   (a root constant buffer view or unordered access view: a buffer allocation and the offset of its range)
//   SetRootTable     parameter, first descriptor   (the descriptor table of the parameter starts at that descriptor of the heap of its type)
//   ResourceBarrier  count, then for each: type (0 transition, 1 unordered access, 2 aliasing), allocation_index (kNoAllocation: all, for the last two), state before, state after, flags (1 begin only, 2 end only)
//                    (an allocation made with kAllocTrackState has the state it was left in; a transition names it, and what uses the allocation after that needs it in a state that allows the use)
//   BindUav          slot, allocation_index, offset, size   (a storage buffer for compute shaders, outside a pass: slot below kMaxStorageBuffers, a kAllocUnorderedAccess buffer, offset and size multiples of 4)
//   Dispatch         groups_x, groups_y, groups_z   (outside a pass, with a compute pipeline bound by BindPipeline and every storage slot its shader uses bound; each count at most kMaxDispatchGroups)
//   ResolveRegion    destination_index, source_index   (outside a pass: the samples of a multisampled colour render target averaged into an ordinary image of the same format and size, the adapter's kFeatureMultisample)
//                    [, mode]: a graphics::ResolveMode; or kResolveCopySamples and then x, y, width, height of the source's region and x, y in the destination: every sample of the region
//                    copied into an image of the same format and samples (kFeature2SampleCopies; Direct3D 12's DECOMPRESS into another resource, depth and stencil too)
//                    [, mode | kResolveSourceLayer, layer]: the resolve of that layer of a multisampled array (kFeature3MultisampleArrays)
//   CopyBuffer       dst_index, dst_offset, src_index, src_offset, bytes   (outside a pass: between two buffer allocations (not heaps; a reserved buffer is read and written as the whole it appears to be; the source may be a kAllocQueryResult buffer: the results of the queries ended before); the ranges lie in the buffers, a copy inside one buffer must not overlap)
//   CopyBufferImage  direction (0 buffer to image, 1 image to buffer), image_index, level, layer, x, y, width, height, buffer_index, buffer_offset, row_pitch   (outside a pass: a rectangle of one level of a colour image
//                    (any kind: `layer` is the array slice or volume depth, 0 for a 2D image) and rows of the buffer, `row_pitch` bytes apart from `buffer_offset` on (at least width * the bytes of a pixel); a multisampled or depth image is refused)
//                    [, narrow]   (a NarrowLayout: the buffer's pixels are narrower than the image's - one or two 8-bit channels of an A8B8G8R8 / X8B8G8R8 image, or one or two 16-bit channels of an
//                    A16B16G16R16 one: copied into the image they become (r, g or 0, 0, maximum), out of it the first channels are taken. What a runtime keeps R8 / R8G8 / R16 / R16G16 textures and
//                    the planes of NV12 / P010 in, as the kernel has no narrower formats; or NarrowLayout::R1 - a bit a pixel, rows of (width + 7) / 8 bytes - of an R8Unorm, A8B8G8R8 or
//                    X8B8G8R8 image: Direct3D's R1_UNORM)
//                    (a depth buffer of one sample takes one of the depth layouts instead - and needs one -, the adapter's kFeature2DepthCopies: the buffer holds its depth as 32-bit floats,
//                    16-bit or 24-bit normalised integers (the 24 bits in the low bits of a word whose high byte is 0, and is not read), or the stencil of a D24S8 buffer as bytes; copied
//                    into the buffer a depth is rounded to the nearest code, into the image a code becomes the float it stands for)
//   CopyTiles        direction (0 buffer to tiles, 1 tiles to buffer), reserved_index, first_tile, tile_count, buffer_index, buffer_offset   (outside a pass: tiles first_tile ..
//                    first_tile + tile_count - 1 of a reserved buffer or texture (VidMm ReservedLayout's numbering) as the 64 KiB each holds in its heap, one after the other in the buffer from
//                    buffer_offset on - Direct3D's CopyTiles. A tile mapped nowhere reads as zeros and drops what is written to it; a plain buffer, not a heap or a reserved one)
//   DecodeFrame      decoder handle, buffer_index, picture_offset, picture_bytes, matrix_offset, matrix_bytes, slices_offset, slices_bytes, bitstream_index, bitstream_offset, bitstream_bytes,
//                    luma_index, chroma_index, status_index, status_offset, output_keyed, key_count, then key_count allocation indices
//                    [, kDecodeConversion, output format (hardware::VpFormat), output colour space, decode colour space, output plane0_index, plane1_index (kNoAllocation for
//                    one image), output width, height]   (decode conversion: the picture is decoded into the surface above - the reference - and then converted into the
//                    output, scaled to width x height, by the video processor; the output is kVideoDecodeWrite. A picture that is not decoded leaves it as it was)
//                    [, kDecodeHistogram, components (bits: Y, U, V, A), bin count, counter bits (32), then 4 pairs of buffer_index (kNoAllocation: none), offset]
//                    (histograms of the decoded picture, as hardware::vp_histogram counts them, into each component's buffer: bin count little-endian 32-bit counters at the
//                    offset; the buffers kVideoDecodeWrite, written only when the picture was decoded)   (outside a pass: decodes one picture with a video decoder of the
//                    device (Kernel::create_video_decoder) - its DXVA picture parameters, inverse quantization matrices and slice controls are the three ranges of the buffer `buffer_index`
//                    (a range of 0 bytes: none), the compressed data the range of `bitstream_index` - into the two planes of the output surface (2D images of one level, the formats of
//                    DxgTypes.hpp VideoDecoderDesc, the luma at least the decoder's size, the chroma at least half of it; a packed surface format is the one image `luma_index`,
//                    `chroma_index` kNoAllocation). Key i is the luma allocation of the surface index i of the
//                    parameters (the current picture's and the references': what the decoder knows each decoded picture by; kNoAllocation for an index not used); output_keyed 1: the
//                    current picture is known by its output (`luma_index`) whatever its index (Direct3D 12), 0: by the key of its index (Direct3D 11). status_index
//                    (kNoAllocation: none) is a buffer the 16 bytes of a VideoDecodeStatus go to, at status_offset. A picture that cannot be decoded leaves the planes as they were and
//                    says so in the status: the stream goes on)
//   VideoProcess     output_format, output_color_space, target left, top, right, bottom, background r, g, b, a (float bits), background_is_yuv, alpha_fill, alpha_fill_stream,
//                    output_plane0_index, output_plane1_index, stream_count, then per stream kVideoProcessStreamWords words: format, color_space, source left, top, right, bottom,
//                    destination left, top, right, bottom, orientation, alpha_blend, alpha (float bits), plane0_index, plane1_index   (outside a pass: Hardware/VideoCodec/VideoProcess.hpp
//                    on the CPU. Formats are hardware::VpFormat; a planar YUV image is two 2D images of one level as the runtime keeps NV12 / P010 / P016 / P208 (DxgTypes.hpp
//                    VideoDecoderDesc), an RGB one a single image of the matching kernel format (Bgra8 A8R8G8B8, Bgrx8 X8R8G8B8, Rgba8 A8B8G8R8, Rgb10a2 A2B10G10R10, Rgba16f
//                    A16B16G16R16F), a packed YUV one a single image as VideoDecoderDesc keeps AYUV, Y410, Y416, YUY2, Y210, Y216; plane1_index is kNoAllocation for one image. At most kMaxVideoProcessStreams streams. A frame the processor refuses (rectangles, colour spaces) leaves the output as it was)
//                    [, kVideoProcessExtension, the display's light levels (VpHdrMetadata: max_mastering, min_mastering, max_content, max_frame_average, float bits), then per
//                    stream kVideoProcessStreamExtWords words: field_order, field, deinterlace, past plane0_index, plane1_index, future plane0_index, plane1_index
//                    (kNoAllocation: none), stereo format, stereo frame, stereo flip, stereo offset (signed), brightness, contrast, hue, saturation, noise reduction,
//                    edge enhancement, anamorphic (float bits), luma_key, lower, upper (float bits), the content's light levels (4 float bits)]   (the optional part: the
//                    VpStream values; without it every stream is progressive, unfiltered and not stereo. The references are pictures like the stream's. Deinterlace 3 is
//                    motion compensated.) With kVideoProcessExtension2 instead each stream's kVideoProcessStreamExtWords are followed by kVideoProcessStreamExt2Words more:
//                    temporal (hardware::VpTemporal), blend (float bits), inverse telecine cycle, drop, output, window count (at most kVpMaxTelecineFrames), then
//                    kVpMaxTelecineFrames pairs of plane0_index, plane1_index (the window's frames, kNoAllocation: not known), stereo adjustment (signed), automatic
//                    processing (kVpAuto* bits), HDR10+ metadata bytes (at most kVpHdr10PlusMaxBytes), then 18 words of them (byte k in bits 8 (k % 4) of word k / 4;
//                    metadata vp_parse_hdr10plus refuses is not used)
//   EstimateMotion   block (8 or 16), width, height, current_luma_index, reference_luma_index, hint_index (kNoAllocation: none), hint_offset, heap_index, heap_offset   (outside a
//                    pass: the motion vectors of the current picture's blocks into the reference (VideoEncode.hpp run_motion_estimation; the lumas are widened planes of at least
//                    width x height, the heaps buffers: hint is an earlier heap, heap receives motion_vector_heap_bytes); the planes kVideoEncodeRead)
//   ResolveMotionVectors heap_index, heap_offset, width, height, image_index, x, y   (outside a pass: the heap's vectors as R16G16_SINT pixels - an A16B16G16R16Sint image of one
//                    level, (x, y, 0, 1) - at (x, y) of the image, which is kVideoEncodeWrite)
//   EncodeFrame      args_index, args_offset, args_bytes, heap_index (kNoAllocation: none), heap_offset, input_luma, input_chroma, recon_luma, recon_chroma (kNoAllocation:
//                    none), bitstream_index, bitstream_offset, metadata_index, metadata_offset, reference_count, then per reference its luma and chroma indices   (outside a
//                    pass: codes one picture as VideoEncode.hpp says - the arguments are the args range, the heap (to the end of its buffer) holds at least kVideoEncodeHeapBytes of
//                    state, and H.264's reference store if it has room (video_encode_heap_bytes); the input and references
//                    kVideoEncodeRead, the reconstruction, bitstream and metadata kVideoEncodeWrite; the planes 8-bit widened, or A16B16G16R16 for HEVC / AV1 at 10 bits. A picture that cannot be coded is said so in the metadata)
//   ResolveEncoderMetadata metadata_index, metadata_offset, out_index, out_offset[, frame_psnr_index, frame_psnr_offset, subregion_psnr_index, subregion_psnr_offset]
//                    (outside a pass: EncodeFrame's metadata as D3D12_VIDEO_ENCODER_OUTPUT_METADATA and its slice records into the buffer `out`, and the PSNRs into the other two
//                    when given - kNoAllocation for none; each kVideoEncodeWrite)
//   BindUavImage     slot, allocation_index, level, first_layer, layer_count [, view format]   (a storage image: u<slot> of a shader that declares it typed, slot below kMaxStorageImages; a kAllocStorageImage
//                    allocation, a level it has and layers it has (an array's layers, a volume's slices); the view's Format, 0 (or left out) the allocation's
//                    own, another wide format of the same pixel size for a wide allocation. Outside a pass, and in one with the adapter's kFeature2StorageImagesInDraws;
//                    kAllocationIndex kNoAllocation: none. What the shaders write is in the image after the dispatch or the draw)
//   BindTexelBuffer  slot, allocation_index, first_element, element_count, format   (a typed view of a buffer, the adapter's kFeature2TexelBuffers: image slot `slot` - below kMaxStorageImages
//                    the u# of a shader that declares a typed buffer (RWBuffer<>), from kTexelBufferSlotBase the t# ones (Buffer<>, slot kTexelBufferSlotBase + n), read only - is `element_count`
//                    elements of `format` (a graphics::TexelFormat: DXGI_FORMAT's number; a u# view one texel_format_storable says) from element `first_element` of a kAllocUnorderedAccess buffer,
//                    within it. Outside a pass, and in one with kFeature2StorageImagesInDraws; kNoAllocation: none. The counter of an append, consume or counter view u<n> is BindUav's slot
//                    kCounterSlotBase + n: a word of a kAllocUnorderedAccess buffer)
//   SetDepthBounds   enable, min, max   (the floats as bits, each 0 .. 1: the depth bounds test of the draws that follow - the adapter's kFeature2DepthBounds -, inside or outside a pass;
//                    the stream starts with it off)
//   SetSamplePositions samples, pixels, then samples * pixels words of x | y << 8 (signed bytes, -8 .. 7: sixteenths of a pixel from the centre)   (kFeature2SamplePositions: the positions of
//                    the samples of the draws that follow, one pattern - pixels 1 - or one for each pixel of a 2 x 2 quad - pixels 4, at most 4 samples; pixels 0 with no positions: the
//                    standard ones again, as the stream starts with)
//   SetViewInstance  view, viewport_offset, layer_offset   (kFeature2ViewInstancing: the SV_ViewID of the draws that follow, below kMaxViewInstances, and what is added to the viewport (below 16)
//                    and the render target array index (below 2048) of each primitive; the stream starts with 0, 0, 0)
//   SetShadingRate   rate, primitive_combiner, image_combiner, allocation_index   (kFeature2ShadingRate: coarse pixel shading of the draws that follow - a D3D12_SHADING_RATE, two
//                    D3D12_SHADING_RATE_COMBINERs, and the shading-rate image, an R8Uint texture of one level, layer and sample (kNoAllocation: none); the stream starts with 1x1,
//                    passthrough, passthrough, none)
//   EndQuery         [allocation_index, offset]   (ends the open query that writes there - with no words, the one begun last that is still open -; what it counted since its BeginQuery (for an
//                    occlusion query the fragments that passed the depth and stencil tests: a little-endian 64-bit number a value) is written there once the stream has run)
#pragma once
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include <initializer_list>
#include "Utility/ByteBuffer.hpp"

namespace dxgk {

// BeginQuery's kind word: bits 0-7 a graphics::QueryKind or one of these kinds of the kernel's own, bits 8-9 the output stream (stream output kinds), bit 16 kQueryAccumulate.
constexpr uint32_t kQueryKindPipelineStatistics1 = 12;   // D3D12_QUERY_DATA_PIPELINE_STATISTICS1 (14 values)
constexpr uint32_t kQueryKindSoStatistics = 13;          // D3D12_QUERY_DATA_SO_STATISTICS (2 values: written, needed)
constexpr uint32_t kQueryAccumulate = 1u << 16;

enum class Op : uint16_t {
  BeginPass = 1,
  EndPass,
  BindPipeline,
  BindVertexBuffer,
  BindIndexBuffer,
  PushConstants,
  BindTexture,
  SetViewport,
  SetScissor,
  Draw,
  DrawIndexed,
  CopyRegion,
  GenerateMipmaps,
  SetTopology,
  DrawInstanced,
  DrawIndexedInstanced,
  BeginQuery,
  EndQuery,
  BindUav,
  Dispatch,
  ResolveRegion,
  SetStreamOutput,
  SetRootSignature,
  SetDescriptorHeaps,
  SetRootConstants,
  SetRootDescriptor,
  SetRootTable,
  ResourceBarrier,
  CopyBuffer,
  CopyBufferImage,
  DecodeFrame,
  VideoProcess,
  EstimateMotion,
  ResolveMotionVectors,
  EncodeFrame,
  ResolveEncoderMetadata,
  BindUavImage,   // (37)
  BindTexelBuffer,   // (38)
  // (the optional rasterizer features of Direct3D 12, numbered apart from the ops other changes add after 38)
  SetDepthBounds = 56,
  SetSamplePositions = 57,
  SetViewInstance = 58,
  SetShadingRate = 59,
  CopyTiles = 48,    // (numbered apart from the ones other work is adding)
};
constexpr uint32_t kMaxViewInstances = 4;   // D3D12_MAX_VIEW_INSTANCE_COUNT
constexpr uint32_t kResolveCopySamples = 16;   // ResolveRegion's mode word for a copy of samples (past the graphics::ResolveMode values)
// ResolveRegion's mode word with this bit: one more word follows, the layer of the multisampled source that is resolved (an array of them: graphics::kFeature3MultisampleArrays).
constexpr uint32_t kResolveSourceLayer = 1u << 8;
constexpr uint32_t kVideoProcessStreamWords = 15;
constexpr uint32_t kVideoProcessExtension = 0x31585056;   // 'VPX1': VideoProcess's optional part follows
constexpr uint32_t kVideoProcessStreamExtWords = 25;
constexpr uint32_t kVideoProcessExtension2 = 0x32585056;   // 'VPX2': the optional part with kVideoProcessStreamExt2Words more a stream
constexpr uint32_t kVideoProcessStreamExt2Words = 43;
constexpr uint32_t kMaxVideoProcessStreams = 16;
constexpr uint32_t kVideoProcessMaxWords = 16 + kVideoProcessStreamWords * kMaxVideoProcessStreams + 5 + (kVideoProcessStreamExtWords + kVideoProcessStreamExt2Words) * kMaxVideoProcessStreams;
constexpr uint32_t kDecodeConversion = 0x31564344;   // 'DCV1': DecodeFrame's conversion follows
constexpr uint32_t kDecodeHistogram = 0x31474844;    // 'DHG1': DecodeFrame's histograms follow

// How the vertices (or indices) of a draw make triangles (D3D10 IASetPrimitiveTopology / D3D9 primitive types); the default is a list. A strip or fan is expanded to a list of
// triangles by the kernel, so any device draws it; with an index buffer the index 0xFFFF (0xFFFFFFFF for 32-bit indices) ends a strip and starts the next.
enum class Topology : uint32_t { TriangleList = 0, TriangleStrip = 1, TriangleFan = 2, LineList = 3, LineStrip = 4, PointList = 5, LineListAdjacency = 0x100, TriangleListAdjacency = 0x101 };   // (the adjacency lists need a geometry shader that takes them: kFeatureGeometryShaders)   // (lines and points: kFeatureLines of the adapter)

// A list of patches of `n` control points (1 .. graphics::kMaxPatchControlPoints), for a pipeline with a hull and a domain shader (the adapter's kFeatureTessellation): Topology value kPatchListBase + n.
constexpr uint32_t kPatchListBase = 5;
inline Topology patch_list(uint32_t control_points) { return static_cast<Topology>(kPatchListBase + control_points); }
inline bool is_patch_list(Topology t) { return static_cast<uint32_t>(t) > kPatchListBase && static_cast<uint32_t>(t) < 0x100; }
inline bool is_adjacency(Topology t) { return t == Topology::LineListAdjacency || t == Topology::TriangleListAdjacency; }
inline uint32_t patch_points(Topology t) { return static_cast<uint32_t>(t) - kPatchListBase; }
// SetTopology's optional second word: which index ends a strip (Direct3D 12's D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, the pipeline's IBStripCutValue or the command list's
// IASetIndexBufferStripCutValue): none (0xFFFF and 0xFFFFFFFF are vertices like any other), 0xFFFF, 0xFFFFFFFF, or - the default, without the word - the largest index of the
// index buffer's size (Direct3D 10 and 11). Strips and fans are cut (a fan begins again at the next index); the lists and patches take every index as a vertex.
enum class StripCut : uint32_t { None = 0, Cut16 = 1, Cut32 = 2, IndexMax = 3 };
// Whether index `raw` of an index buffer of `index_bytes` ends a strip.
constexpr bool is_strip_cut(StripCut cut, uint32_t index_bytes, uint32_t raw) {
  switch (cut) {
    case StripCut::None: return false;
    case StripCut::Cut16: return raw == 0xFFFFu;
    case StripCut::Cut32: return raw == 0xFFFFFFFFu;
    default: return raw == (index_bytes == 4 ? 0xFFFFFFFFu : 0xFFFFu);
  }
}

constexpr uint32_t kNoAllocation = 0xFFFFFFFFu;
// CopyBufferImage's last word: how a buffer's pixels are narrower than the image's - or, of a depth buffer, which plane the buffer holds and how (DepthFloat32 .. Stencil8).
// R1: a bit a pixel, the first pixel of a row in the most significant bit of its first byte (Direct3D's R1_UNORM), of an R8Unorm image or an A8B8G8R8 / X8B8G8R8 one: copied into the
// image a pixel becomes 0 or the maximum in red (the widened one's alpha the maximum), out of it a red of at least half is a 1.
enum class NarrowLayout : uint32_t { None = 0, R8 = 1, R8G8 = 2, R16 = 3, R16G16 = 4, DepthFloat32 = 5, DepthUnorm16 = 6, DepthUnorm24 = 7, Stencil8 = 8, R1 = 9 };
constexpr bool is_depth_layout(NarrowLayout n) { return n == NarrowLayout::DepthFloat32 || n == NarrowLayout::DepthUnorm16 || n == NarrowLayout::DepthUnorm24 || n == NarrowLayout::Stencil8; }
constexpr uint32_t narrow_pixel_bytes(NarrowLayout n) {   // (R1: 0, a bit a pixel)
  return n == NarrowLayout::R8 || n == NarrowLayout::Stencil8 ? 1 : n == NarrowLayout::R8G8 || n == NarrowLayout::R16 || n == NarrowLayout::DepthUnorm16 ? 2
         : n == NarrowLayout::R16G16 || n == NarrowLayout::DepthFloat32 || n == NarrowLayout::DepthUnorm24 ? 4 : 0;
}
constexpr uint32_t kStreamOutputAppendOffset = 0xFFFFFFFFu;   // SetStreamOutput's offset for "after what the last stream output into the buffer wrote"
constexpr uint32_t kStreamOutputNoEnd = 0xFFFFFFFFu;          // SetStreamOutput's end for "the buffer's end"
// (the longest packets: PushConstants - the byte count, then up to kMaxPushConstantBytes of constants - and a VideoProcess of every stream with both optional parts)
constexpr uint32_t kMaxPacketWords = 1 + (1 + graphics::kMaxPushConstantBytes / 4 > kVideoProcessMaxWords ? 1 + graphics::kMaxPushConstantBytes / 4 : kVideoProcessMaxWords);
constexpr uint64_t kMaxCommandBytes = 16 * 1024 * 1024;

enum BeginPassFlags : uint32_t { kPassClearColor = 1u << 0, kPassClearDepth = 1u << 1, kPassClearStencil = 1u << 2, kPassLayers = 1u << 3, kPassLevels = 1u << 4 };

inline uint32_t float_bits(float f) { return __builtin_bit_cast(uint32_t, f); }
inline float bits_float(uint32_t w) { return __builtin_bit_cast(float, w); }

// What a user-mode driver (and the tests) build a stream with. Every call returns false once memory ran out; `ok()` says whether the stream is whole.
class CommandWriter {
 public:
  explicit CommandWriter(allocators::IAllocator& allocator) : bytes_(allocator) {}

  bool ok() const { return ok_; }
  ConstByteView view() const { return bytes_.view(); }
  void clear() { bytes_.clear(); ok_ = true; }

  void begin_pass(uint32_t color, uint32_t depth, uint32_t flags, graphics::Vec4 clear_color, float clear_depth, uint32_t clear_stencil) {
    packet(Op::BeginPass, {color, depth, flags, float_bits(clear_color.x), float_bits(clear_color.y), float_bits(clear_color.z), float_bits(clear_color.w), float_bits(clear_depth), clear_stencil});
  }
  // A pass with several colour targets (the adapter's kFeatureMultipleTargets): `extra` are the allocation indices of targets 1 .. n (at most three).
  void begin_pass_targets(uint32_t color, std::initializer_list<uint32_t> extra, uint32_t depth, uint32_t flags, graphics::Vec4 clear_color, float clear_depth, uint32_t clear_stencil) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::BeginPass) | ((10 + static_cast<uint32_t>(extra.size())) << 16));
    for (uint32_t w : {color, depth, flags, float_bits(clear_color.x), float_bits(clear_color.y), float_bits(clear_color.z), float_bits(clear_color.w), float_bits(clear_depth), clear_stencil})
      ok_ &= bytes_.put32(w);
    for (uint32_t w : extra) ok_ &= bytes_.put32(w);
  }
  // A pass that draws into layers of its targets (kPassLayers): `layers` has a first layer and a count for each colour target (color, then `extra`) and then for the depth buffer.
  void begin_pass_layers(uint32_t color, std::initializer_list<uint32_t> extra, uint32_t depth, uint32_t flags, graphics::Vec4 clear_color, float clear_depth, uint32_t clear_stencil,
                         std::initializer_list<uint32_t> layers) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::BeginPass) | ((10 + static_cast<uint32_t>(extra.size() + layers.size())) << 16));
    for (uint32_t w : {color, depth, flags | kPassLayers, float_bits(clear_color.x), float_bits(clear_color.y), float_bits(clear_color.z), float_bits(clear_color.w), float_bits(clear_depth), clear_stencil})
      ok_ &= bytes_.put32(w);
    for (uint32_t w : extra) ok_ &= bytes_.put32(w);
    for (uint32_t w : layers) ok_ &= bytes_.put32(w);
  }
  // A pass that draws into levels of its targets (kPassLevels, and kPassLayers when `layers` is not empty): `levels` has the level of each colour target and then of the depth buffer.
  void begin_pass_levels(uint32_t color, std::initializer_list<uint32_t> extra, uint32_t depth, uint32_t flags, graphics::Vec4 clear_color, float clear_depth, uint32_t clear_stencil,
                         std::initializer_list<uint32_t> layers, std::initializer_list<uint32_t> levels) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::BeginPass) | ((10 + static_cast<uint32_t>(extra.size() + layers.size() + levels.size())) << 16));
    for (uint32_t w : {color, depth, flags | kPassLevels | (layers.size() ? kPassLayers : 0u), float_bits(clear_color.x), float_bits(clear_color.y), float_bits(clear_color.z), float_bits(clear_color.w),
                       float_bits(clear_depth), clear_stencil})
      ok_ &= bytes_.put32(w);
    for (uint32_t w : extra) ok_ &= bytes_.put32(w);
    for (uint32_t w : layers) ok_ &= bytes_.put32(w);
    for (uint32_t w : levels) ok_ &= bytes_.put32(w);
  }
  void end_pass() { packet(Op::EndPass, {}); }
  void bind_pipeline(Handle pipeline) { packet(Op::BindPipeline, {pipeline}); }
  void bind_vertex_buffer(uint32_t allocation, uint32_t offset = 0) { packet(Op::BindVertexBuffer, {allocation, offset}); }
  void bind_vertex_buffer_slot(uint32_t slot, uint32_t allocation, uint32_t offset = 0) { packet(Op::BindVertexBuffer, {allocation, offset, slot}); }
  void bind_index_buffer(uint32_t allocation, graphics::IndexType type, uint32_t offset = 0) { packet(Op::BindIndexBuffer, {allocation, type == graphics::IndexType::Uint32 ? 1u : 0u, offset}); }
  void push_constants(ConstByteView data) {
    const uint32_t words = static_cast<uint32_t>((data.size() + 3) / 4);
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::PushConstants) | ((2 + words) << 16));
    ok_ &= bytes_.put32(static_cast<uint32_t>(data.size()));
    ok_ &= bytes_.append(data);
    ok_ &= bytes_.put_zeros(uint64_t{words} * 4 - data.size());
  }
  void bind_texture(uint32_t unit, uint32_t allocation, const graphics::SamplerDesc& s) {
    if (sampler_extras(s)) {
      bind_texture_view(unit, allocation, s, 0, 0, 0, 0);   // (the sampler's extra words come after the view's: all of the image)
      return;
    }
    packet(Op::BindTexture, {unit, allocation, filter_word(s), address_word(s.address_u, s.border), address_word(s.address_v), static_cast<uint32_t>(s.mip_filter),
                             float_bits(s.lod_bias), float_bits(s.min_lod), float_bits(s.max_lod), texture_view_word(s)});
  }
  // The same through a view (kFeature2MipmappedLayers): levels base_level .., layers first_layer .. (counts of 0: to the last). With the sampler's anisotropy or border colour of
  // its own (kFeature2SamplerState) the six words of those follow.
  void bind_texture_view(uint32_t unit, uint32_t allocation, const graphics::SamplerDesc& s, uint32_t base_level, uint32_t level_count, uint32_t first_layer, uint32_t layer_count) {
    if (sampler_extras(s)) {
      packet(Op::BindTexture, {unit, allocation, filter_word(s), address_word(s.address_u, s.border), address_word(s.address_v), static_cast<uint32_t>(s.mip_filter),
                               float_bits(s.lod_bias), float_bits(s.min_lod), float_bits(s.max_lod), texture_view_word(s), base_level, level_count, first_layer, layer_count,
                               s.max_anisotropy, s.custom_border ? 1u : 0u, float_bits(s.border_rgba[0]), float_bits(s.border_rgba[1]), float_bits(s.border_rgba[2]),
                               float_bits(s.border_rgba[3])});
      return;
    }
    packet(Op::BindTexture, {unit, allocation, filter_word(s), address_word(s.address_u, s.border), address_word(s.address_v), static_cast<uint32_t>(s.mip_filter),
                             float_bits(s.lod_bias), float_bits(s.min_lod), float_bits(s.max_lod), texture_view_word(s), base_level, level_count, first_layer, layer_count});
  }
  static constexpr bool sampler_extras(const graphics::SamplerDesc& s) { return s.max_anisotropy > 1 || s.custom_border; }
  void set_viewport(const graphics::Viewport& v) { packet(Op::SetViewport, {float_bits(v.x), float_bits(v.y), float_bits(v.width), float_bits(v.height), float_bits(v.min_depth), float_bits(v.max_depth)}); }
  void set_scissor(const graphics::Rect2D& r) { packet(Op::SetScissor, {static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), r.width, r.height}); }
  // Several viewports and scissor rectangles (kFeature2Viewports): 1 .. graphics::kMaxViewports of them.
  void set_viewports(ArrayView<const graphics::Viewport> list) {
    ok_ &= list.size() >= 1 && list.size() <= graphics::kMaxViewports;
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::SetViewport) | ((1 + 6 * static_cast<uint32_t>(list.size())) << 16));
    for (const graphics::Viewport& v : list)
      for (uint32_t w : {float_bits(v.x), float_bits(v.y), float_bits(v.width), float_bits(v.height), float_bits(v.min_depth), float_bits(v.max_depth)}) ok_ &= bytes_.put32(w);
  }
  void set_scissors(ArrayView<const graphics::Rect2D> list) {
    ok_ &= list.size() >= 1 && list.size() <= graphics::kMaxViewports;
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::SetScissor) | ((1 + 4 * static_cast<uint32_t>(list.size())) << 16));
    for (const graphics::Rect2D& r : list)
      for (uint32_t w : {static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), r.width, r.height}) ok_ &= bytes_.put32(w);
  }
  void set_topology(Topology topology) { packet(Op::SetTopology, {static_cast<uint32_t>(topology)}); }
  void set_topology(Topology topology, StripCut cut) { packet(Op::SetTopology, {static_cast<uint32_t>(topology), static_cast<uint32_t>(cut)}); }
  void draw(uint32_t vertex_count, uint32_t first_vertex = 0) { packet(Op::Draw, {vertex_count, first_vertex}); }
  // Several instances (the adapter's kFeatureInstancing): the draw is made instance_count times, numbered from first_instance; per-instance attributes step with them.
  void draw_instanced(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex = 0, uint32_t first_instance = 0) { packet(Op::DrawInstanced, {vertex_count, first_vertex, instance_count, first_instance}); }
  void draw_indexed_instanced(uint32_t index_count, uint32_t instance_count, uint32_t first_index = 0, int32_t vertex_offset = 0, uint32_t first_instance = 0) {
    packet(Op::DrawIndexedInstanced, {index_count, first_index, static_cast<uint32_t>(vertex_offset), instance_count, first_instance});
  }
  void draw_indexed(uint32_t index_count, uint32_t first_index = 0, int32_t vertex_offset = 0) { packet(Op::DrawIndexed, {index_count, first_index, static_cast<uint32_t>(vertex_offset)}); }
  void copy_region(uint32_t dst, uint32_t dst_x, uint32_t dst_y, uint32_t src, uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height) {
    packet(Op::CopyRegion, {dst, dst_x, dst_y, src, src_x, src_y, width, height});
  }
  void copy_subresource_region(uint32_t dst, uint32_t dst_level, uint32_t dst_layer, uint32_t dst_x, uint32_t dst_y, uint32_t src, uint32_t src_level, uint32_t src_layer, uint32_t src_x,
                               uint32_t src_y, uint32_t width, uint32_t height, uint32_t layer_count = 1) {
    packet(Op::CopyRegion, {dst, dst_x, dst_y, src, src_x, src_y, width, height, dst_level, dst_layer, src_level, src_layer, layer_count});
  }
  // A whole subresource (`layer_count` layers) of a depth buffer or a multisampled image into another (kFeature2DepthCopies): the planes of a depth buffer (bit 0 the depth, bit 1 the stencil;
  // 0 every one), the size of the destination's level.
  void copy_whole_subresources(uint32_t dst, uint32_t dst_level, uint32_t dst_layer, uint32_t src, uint32_t src_level, uint32_t src_layer, uint32_t width, uint32_t height, uint32_t layer_count = 1,
                               uint32_t planes = 0) {
    packet(Op::CopyRegion, {dst, 0, 0, src, 0, 0, width, height, dst_level, dst_layer, src_level, src_layer, layer_count, planes});
  }
  void generate_mipmaps(uint32_t allocation) { packet(Op::GenerateMipmaps, {allocation}); }
  // `kind`: the kind word (0, the occlusion count, when it is left out: the packet then has two words)
  void begin_query(uint32_t allocation, uint32_t offset = 0, uint32_t kind = 0) {
    if (kind == 0) packet(Op::BeginQuery, {allocation, offset});
    else packet(Op::BeginQuery, {allocation, offset, kind});
  }
  // Ends the query begun last that is still open, or the open one that writes at `offset` of `allocation`.
  void end_query() { packet(Op::EndQuery, {}); }
  void end_query(uint32_t allocation, uint32_t offset) { packet(Op::EndQuery, {allocation, offset}); }
  // Stream output buffer `slot` (below kMaxStreamOutputBuffers) is the kAllocStreamOutput allocation, written from `offset` (a multiple of 4; the draws that follow append); kNoAllocation unbinds it.
  // (`offset` kStreamOutputAppendOffset: where the last stream output into the buffer ended)
  void set_stream_output(uint32_t slot, uint32_t allocation, uint32_t offset = 0) { packet(Op::SetStreamOutput, {slot, allocation, offset}); }
  // (and the byte its writes stop at: `end`)
  void set_stream_output(uint32_t slot, uint32_t allocation, uint32_t offset, uint32_t end) { packet(Op::SetStreamOutput, {slot, allocation, offset, end}); }
  void set_root_signature(Handle signature) { packet(Op::SetRootSignature, {signature}); }
  void set_descriptor_heaps(std::initializer_list<Handle> heaps) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::SetDescriptorHeaps) | ((2 + static_cast<uint32_t>(heaps.size())) << 16));
    ok_ &= bytes_.put32(static_cast<uint32_t>(heaps.size()));
    for (Handle h : heaps) ok_ &= bytes_.put32(h);
  }
  void set_root_constants(uint32_t parameter, uint32_t first, ConstByteView values) {
    const uint32_t count = static_cast<uint32_t>(values.size() / 4);
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::SetRootConstants) | ((4 + count) << 16));
    ok_ &= bytes_.put32(parameter);
    ok_ &= bytes_.put32(first);
    ok_ &= bytes_.put32(count);
    ok_ &= bytes_.append(values.first(uint64_t{count} * 4));
  }
  void set_root_descriptor(uint32_t parameter, uint32_t allocation, uint32_t offset = 0) { packet(Op::SetRootDescriptor, {parameter, allocation, offset}); }
  void set_root_table(uint32_t parameter, uint32_t first_descriptor) { packet(Op::SetRootTable, {parameter, first_descriptor}); }
  struct Barrier {
    uint32_t type = 0, allocation = kNoAllocationIndex, before = 0, after = 0, flags = 0;
    static constexpr uint32_t kNoAllocationIndex = 0xFFFFFFFFu;
  };
  void resource_barrier(std::initializer_list<Barrier> barriers) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::ResourceBarrier) | ((2 + 5 * static_cast<uint32_t>(barriers.size())) << 16));
    ok_ &= bytes_.put32(static_cast<uint32_t>(barriers.size()));
    for (const Barrier& b : barriers)
      for (uint32_t w : {b.type, b.allocation, b.before, b.after, b.flags}) ok_ &= bytes_.put32(w);
  }
  void transition(uint32_t allocation, uint32_t before, uint32_t after) { resource_barrier({Barrier{0, allocation, before, after, 0}}); }
  void copy_buffer(uint32_t dst, uint32_t dst_offset, uint32_t src, uint32_t src_offset, uint32_t bytes) { packet(Op::CopyBuffer, {dst, dst_offset, src, src_offset, bytes}); }
  void copy_buffer_to_image(uint32_t image, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t buffer, uint32_t offset, uint32_t row_pitch) {
    packet(Op::CopyBufferImage, {0, image, level, layer, x, y, width, height, buffer, offset, row_pitch});
  }
  void copy_image_to_buffer(uint32_t image, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t buffer, uint32_t offset, uint32_t row_pitch) {
    packet(Op::CopyBufferImage, {1, image, level, layer, x, y, width, height, buffer, offset, row_pitch});
  }
  // The same with the buffer's pixels narrower than the image's (NarrowLayout): direction 0 buffer to image, 1 image to buffer.
  void copy_buffer_image_narrow(uint32_t direction, uint32_t image, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t buffer, uint32_t offset,
                                uint32_t row_pitch, NarrowLayout narrow) {
    packet(Op::CopyBufferImage, {direction, image, level, layer, x, y, width, height, buffer, offset, row_pitch, static_cast<uint32_t>(narrow)});
  }
  void copy_tiles(uint32_t direction, uint32_t reserved, uint32_t first_tile, uint32_t tile_count, uint32_t buffer, uint32_t offset) {
    packet(Op::CopyTiles, {direction, reserved, first_tile, tile_count, buffer, offset});
  }
  void bind_uav(uint32_t slot, uint32_t allocation, uint32_t offset, uint32_t size) { packet(Op::BindUav, {slot, allocation, offset, size}); }
  void bind_uav_image(uint32_t slot, uint32_t allocation, uint32_t level = 0, uint32_t first_layer = 0, uint32_t layer_count = 1, Format view = Format::Unknown) {
    packet(Op::BindUavImage, {slot, allocation, level, first_layer, layer_count, static_cast<uint32_t>(view)});
  }
  void bind_texel_buffer(uint32_t slot, uint32_t allocation, uint32_t first_element, uint32_t element_count, graphics::TexelFormat format) {
    packet(Op::BindTexelBuffer, {slot, allocation, first_element, element_count, static_cast<uint32_t>(format)});
  }
  void set_depth_bounds(bool enable, float min, float max) { packet(Op::SetDepthBounds, {enable ? 1u : 0u, float_bits(min), float_bits(max)}); }
  void set_sample_positions(uint32_t samples, uint32_t pixels, ArrayView<const graphics::SamplePosition> positions) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::SetSamplePositions) | ((3 + static_cast<uint32_t>(positions.size())) << 16)) && bytes_.put32(samples) && bytes_.put32(pixels);
    for (const graphics::SamplePosition& at : positions) ok_ &= bytes_.put32(static_cast<uint8_t>(at.x) | static_cast<uint32_t>(static_cast<uint8_t>(at.y)) << 8);
  }
  void set_view_instance(uint32_t view, uint32_t viewport_offset, uint32_t layer_offset) { packet(Op::SetViewInstance, {view, viewport_offset, layer_offset}); }
  void set_shading_rate(uint32_t rate, uint32_t primitive_combiner, uint32_t image_combiner, uint32_t image = kNoAllocation) {
    packet(Op::SetShadingRate, {rate, primitive_combiner, image_combiner, image});
  }
  struct DecodeArgs {
    Handle decoder = 0;
    uint32_t buffer = kNoAllocation, picture_offset = 0, picture_bytes = 0, matrix_offset = 0, matrix_bytes = 0, slices_offset = 0, slices_bytes = 0;
    uint32_t bitstream = kNoAllocation, bitstream_offset = 0, bitstream_bytes = 0;
    uint32_t luma = kNoAllocation, chroma = kNoAllocation, status = kNoAllocation, status_offset = 0;
    uint32_t output_keyed = 0;
  };
  void decode_frame(const DecodeArgs& a, ArrayView<const uint32_t> keys) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(Op::DecodeFrame) | ((18 + static_cast<uint32_t>(keys.size())) << 16));
    for (uint32_t w : {a.decoder, a.buffer, a.picture_offset, a.picture_bytes, a.matrix_offset, a.matrix_bytes, a.slices_offset, a.slices_bytes, a.bitstream, a.bitstream_offset,
                       a.bitstream_bytes, a.luma, a.chroma, a.status, a.status_offset, a.output_keyed, static_cast<uint32_t>(keys.size())})
      ok_ &= bytes_.put32(w);
    for (uint32_t k : keys) ok_ &= bytes_.put32(k);
  }
  void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) { packet(Op::Dispatch, {x, y, z}); }
  // `mode`: a graphics::ResolveMode (0 average, 1 sample 0, 2 least, 3 greatest); a depth buffer needs 1 or the adapter's kFeatureResolveModes. Left out (0, two words), it is the average of a colour target.
  void resolve_region(uint32_t destination, uint32_t source, uint32_t mode = 0) {
    if (mode == 0) packet(Op::ResolveRegion, {destination, source});
    else packet(Op::ResolveRegion, {destination, source, mode});
  }
  // A layer of a multisampled array (kResolveSourceLayer).
  void resolve_region_layer(uint32_t destination, uint32_t source, uint32_t mode, uint32_t source_layer) { packet(Op::ResolveRegion, {destination, source, mode | kResolveSourceLayer, source_layer}); }
  void copy_samples(uint32_t destination, uint32_t source, const graphics::Rect2D& region, uint32_t x, uint32_t y) {
    packet(Op::ResolveRegion, {destination, source, kResolveCopySamples, static_cast<uint32_t>(region.x), static_cast<uint32_t>(region.y), region.width, region.height, x, y});
  }

 private:
  void packet(Op op, std::initializer_list<uint32_t> payload) {
    ok_ &= bytes_.put32(static_cast<uint32_t>(op) | ((1 + static_cast<uint32_t>(payload.size())) << 16));
    for (uint32_t word : payload) ok_ &= bytes_.put32(word);
  }
  ByteBuffer bytes_;
  bool ok_ = true;
};

}  // namespace dxgk
