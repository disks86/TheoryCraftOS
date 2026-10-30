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

// The kernel-mode half of Direct3D (what Windows calls dxgkrnl, and Mesa's gallium-nine gets from the DRM driver underneath it): the types its services use.
//
// Direct3D is two halves. The user-mode driver (a D3D9/D3D11 runtime plus a vendor UMD - nine's state tracker is the free equivalent) turns API calls into a stream of
// commands; the kernel half owns what a user process must not touch directly: the adapters, video memory and who may use which of it, the queues the commands go to, the
// order they run in, the fences between them, and the display. Everything here is named after its D3DKMT counterpart (D3DKMTCreateDevice, D3DKMTCreateAllocation,
// D3DKMTLock, D3DKMTRender, D3DKMTPresent, ...), so the system-call layer that user-mode Windows programs reach it by (win32u's NtGdiDdDDI* services) is a thin one.
//
// Where it differs from Windows, on purpose: the "miniport" below the kernel is a graphics::IGraphicsDevice (the software rasterizer, virgl, SVGA3D, Intel Gen9, ...), so the
// command stream (CommandStream.hpp) is a device-independent one the kernel validates and replays on that device, not a hardware DMA buffer; and nothing here blocks
// by itself - the scheduler runs on the calling thread (a kernel thread can drive it with Kernel::pump()).
#pragma once
#include <stdint.h>
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Graphics/GraphicsTypes.hpp"

namespace dxgk {

using NtStatus = uint32_t;
using Handle = uint32_t;     // D3DKMT_HANDLE: 0 is no handle; a handle belongs to one process
using ProcessId = uint64_t;
constexpr Handle kNullHandle = 0;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

namespace status {
constexpr NtStatus kSuccess = 0;
constexpr NtStatus kTimeout = 0x00000102;
constexpr NtStatus kDeviceBusy = 0x80000011;
constexpr NtStatus kInvalidHandle = 0xC0000008;
constexpr NtStatus kInvalidParameter = 0xC000000D;
constexpr NtStatus kNoMemory = 0xC0000017;
constexpr NtStatus kAccessDenied = 0xC0000022;
constexpr NtStatus kBufferTooSmall = 0xC0000023;
constexpr NtStatus kNotSupported = 0xC00000BB;
constexpr NtStatus kInsufficientResources = 0xC000009A;
constexpr NtStatus kInvalidDeviceState = 0xC0000184;
constexpr NtStatus kNotFound = 0xC0000225;
constexpr NtStatus kDeviceRemoved = 0xC00002B6;
constexpr NtStatus kGraphicsNoVideoMemory = 0xC01E0100;
constexpr NtStatus kGraphicsAllocationBusy = 0xC01E0102;
constexpr NtStatus kGraphicsTryAgainLater = 0xC01E0104;
constexpr NtStatus kGraphicsAllocationInvalid = 0xC01E0106;
inline bool ok(NtStatus s) { return s == kSuccess; }
}  // namespace status

struct Luid {
  uint32_t low = 0;
  int32_t high = 0;
  bool operator==(const Luid& o) const { return low == o.low && high == o.high; }
};

// --- Allocations --------------------------------------------------------------

// The formats of an allocation (D3DDDIFORMAT, the ones the devices can draw with). The memory layout is little-endian 32-bit words: A8R8G8B8 is 0xAARRGGBB, A8B8G8R8
// is 0xAABBGGRR. The X formats ignore the alpha byte (a texture or render target of one reads as opaque).
// A16B16G16R16F and A32B32G32R32F (D3DFMT_A16B16G16R16F / A32B32G32R32F) keep four half or single floats per pixel in the order R, G, B, A, unclamped: they need the adapter's
// kFeatureFloatFormats (except as staging memory), have one mip level, and cannot be presented.
// The sRGB formats (DXGI_FORMAT_B8G8R8A8_UNORM_SRGB / R8G8B8A8_UNORM_SRGB) lay out like A8R8G8B8 / A8B8G8R8 but hold sRGB-encoded colour channels: a texture is read as the linear values they
// mean, a render target takes linear values and stores them encoded (the adapter's kFeatureSrgbFormats; not presentable). A16B16G16R16 (D3DFMT_A16B16G16R16, unorm) keeps four 16-bit
// normalised channels in the order R, G, B, A (kFeatureUnorm16Formats). A32B32G32R32Uint (DXGI_FORMAT_R32G32B32A32_UINT) keeps four unsigned integers (kFeatureIntegerFormats): a shader
// writes them as bits, a texture is read with `ld` (no filtering), the target takes no blending, and the clear colour of a pass is the four integers as float bits.
// Like the floats, these three have one mip level and cannot be presented or multisampled.
enum class Format : uint32_t { Unknown, A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8, D32Float, D24S8, A16B16G16R16F, A32B32G32R32F, A8R8G8B8Srgb, A8B8G8R8Srgb, A16B16G16R16, A32B32G32R32Uint };

constexpr bool is_depth(Format f) { return f == Format::D32Float || f == Format::D24S8; }
constexpr bool is_float(Format f) { return f == Format::A16B16G16R16F || f == Format::A32B32G32R32F; }
constexpr bool is_srgb(Format f) { return f == Format::A8R8G8B8Srgb || f == Format::A8B8G8R8Srgb; }
constexpr bool is_integer(Format f) { return f == Format::A32B32G32R32Uint; }
// Formats that are read and written as Vec4s (no 32-bit words), one level.
constexpr bool is_wide(Format f) { return is_float(f) || f == Format::A16B16G16R16 || is_integer(f); }
constexpr bool is_color(Format f) { return f == Format::A8R8G8B8 || f == Format::X8R8G8B8 || f == Format::A8B8G8R8 || f == Format::X8B8G8R8 || is_srgb(f) || is_wide(f); }
constexpr bool has_alpha(Format f) { return f == Format::A8R8G8B8 || f == Format::A8B8G8R8 || is_srgb(f) || is_wide(f); }
// Bytes of one pixel of a colour format.
constexpr uint32_t pixel_bytes(Format f) { return (f == Format::A16B16G16R16F || f == Format::A16B16G16R16) ? 8 : (f == Format::A32B32G32R32F || f == Format::A32B32G32R32Uint) ? 16 : 4; }
// Red and blue swapped relative to the devices' logical 0xAARRGGBB.
constexpr bool is_rgba_order(Format f) { return f == Format::A8B8G8R8 || f == Format::X8B8G8R8 || f == Format::A8B8G8R8Srgb; }
// A8R8G8B8 and its sRGB twin: the layout the devices take without a conversion.
constexpr bool is_plain_bgra(Format f) { return f == Format::A8R8G8B8 || f == Format::A8R8G8B8Srgb; }

// What an allocation may be used for (D3DDDI_ALLOCATIONINFO / resource flags). A buffer is made with kAllocVertexBuffer and/or kAllocIndexBuffer (or alone, kAllocQueryResult); everything else is an
// image. kAllocStaging is memory only the CPU and copies touch (never resident on the GPU).
enum AllocationFlags : uint32_t {
  kAllocVertexBuffer = 1u << 0,
  kAllocIndexBuffer = 1u << 1,
  kAllocTexture = 1u << 2,       // sampled by shaders
  kAllocRenderTarget = 1u << 3,  // drawn into
  kAllocDepthStencil = 1u << 4,
  kAllocPresent = 1u << 5,       // may be put on the screen (a back buffer or the primary)
  kAllocStaging = 1u << 6,
  kAllocShared = 1u << 7,        // may be shared with other processes (Kernel::share_allocation)
  kAllocUnorderedAccess = 1u << 9,   // a buffer compute shaders read and write (BindUav; the adapter's kFeatureCompute); may also be a vertex or index buffer
  kAllocQueryResult = 1u << 8,   // a buffer the results of occlusion queries are written to (CommandStream.hpp BeginQuery); system memory only, never resident, read by locking it
};

constexpr uint32_t kMaxTextureDimension = 4096;
constexpr uint64_t kMaxBufferBytes = 256ull * 1024 * 1024;

struct AllocationDesc {
  uint32_t flags = 0;
  Format format = Format::Unknown;   // images
  uint32_t width = 0, height = 0;    // images
  uint32_t mip_levels = 1;           // textures only; more than one needs a texture that is not a render target
  uint64_t size = 0;                 // buffers: bytes
  graphics::TextureKind kind = graphics::TextureKind::Texture2D;   // textures: more than 2D needs the adapter's kFeatureTextureKinds, flags exactly kAllocTexture (kAllocShared too), one mip level, an 8-bit colour format
  uint32_t samples = 1;              // 2, 4 or 8 (the adapter's kFeatureMultisample and sample counts): a render target of an 8-bit colour format or a depth buffer, nothing else; it has no system-memory
                                     // backing, cannot be locked, copied, sampled or presented, and is turned into an ordinary image by the stream's ResolveRegion
  uint32_t layers = 1;               // Texture3D: the depth; Texture2DArray: the array size (1 .. graphics::kMaxTextureLayers); TextureCube: 6 (width equals height)
};

struct AllocationInfo {
  AllocationDesc desc;
  uint64_t bytes = 0;      // the system-memory backing (all mip levels)
  uint32_t pitch = 0;      // bytes per row of a level (level 0)
  bool resident = false;
  bool shared = false;
};

// --- Locking --------------------------------------------------------------------

enum LockFlags : uint32_t {
  kLockReadOnly = 1u << 0,   // the CPU will not write: the GPU copy stays valid
  kLockDoNotWait = 1u << 1,  // fail with kGraphicsAllocationBusy instead of waiting for queued work to finish
  kLockDiscard = 1u << 2,    // the old contents are not needed (nothing is read back from the GPU)
};

struct LockResult {
  ByteView data;        // the subresource's memory (a view of the allocation's system-memory backing: valid until unlock)
  uint32_t pitch = 0;   // bytes per row
};

// --- Adapters ---------------------------------------------------------------------

struct AdapterCaps {
  uint32_t max_texture_dimension = 0;
  uint32_t max_vertex_attributes = 0;
  uint32_t max_varyings = 0;
  uint32_t max_texture_units = 0;
  uint32_t max_push_constant_bytes = 0;
  uint32_t max_vertex_stride = 0;
  bool accelerated = false;   // a GPU draws (not the CPU)
  bool stencil = false;       // the devices accept a stencil plane
  bool mipmaps = false;
  uint32_t features = 0;             // graphics::DeviceFeature bits of the device: blend state and depth comparison, texel access, system values, instancing, lines and points
  bool textures = false;             // sampled images (texture units; max_texture_units says how many pipelines may use)
  bool programmable_vertex = false;  // a vertex shader of any program (as a function, TGSI or Direct3D 9 tokens), not only the device's fixed transform
  bool programmable_pixel = false;   // the same for a pixel shader; false: the device has fixed pixel kernels (see D3d9Devices.cpp) and takes only shaders that are exactly one
  char name[64] = {};
};

enum class MemorySegmentGroup : uint32_t { Local, NonLocal };   // D3DKMT_MEMORY_SEGMENT_GROUP
struct VideoMemoryInfo {
  uint64_t budget = 0;            // what the process may use right now
  uint64_t current_usage = 0;     // resident bytes of this process
  uint64_t available_for_reservation = 0;
  uint64_t current_reservation = 0;
};

struct AdapterEntry {
  Handle handle = 0;
  Luid luid;
  uint32_t index = 0;   // adapter number
};

enum class DeviceExecState : uint32_t { Running, ErrorOutOfMemory, Hung, PageFault };   // D3DKMT_DEVICESTATE_EXECUTION
struct DeviceState {
  DeviceExecState execution = DeviceExecState::Running;
  uint32_t reset_count = 0;   // how many times a context of the device was lost
  NtStatus last_fault = 0;    // why the last one was lost
};

enum class ContextPriority : uint8_t { Low = 0, Normal = 1, High = 2 };

// --- Synchronization ------------------------------------------------------------------

// A fence is a 64-bit counter that only goes up; waiting for N completes once it reaches N (the monitored fence of WDDM 2). A semaphore counts signals up to a limit; a wait
// takes one.
enum class SyncType : uint32_t { Fence, Semaphore };

// --- Shaders and pipelines (private driver escapes) ---------------------------------------

// The shaders the kernel can make from what a user-mode driver supplies. There is no shader compiler in the kernel: a shader is the built-in program of the same name
// (BuiltinShaders), the code the back end takes (TGSI text for virgl, Direct3D 9 tokens for SVGA3D), or Direct3D 9 bytecode, which the kernel decodes and interprets
// (D3d9Shader.hpp) for the devices that run native functions (the software rasterizer).
enum class ShaderKind : uint32_t { ColoredVertex, ColoredFragment, TexturedVertex, TexturedFragment, Tgsi, D3d9Tokens, D3d9Bytecode, Dxbc };

struct ShaderDesc {
  ShaderKind kind = ShaderKind::ColoredVertex;
  graphics::ShaderStage stage = graphics::ShaderStage::Vertex;   // for Tgsi and D3d9Tokens
  ConstByteView code;                                            // Tgsi: the text; D3d9Tokens, D3d9Bytecode: little-endian 32-bit tokens (bytecode: the stage is the one it says)
};

struct PipelineDesc {
  Handle vertex_shader = 0, fragment_shader = 0;
  Handle hull_shader = 0, domain_shader = 0;   // not 0: tessellation (both, stages Hull and Domain; needs kFeatureTessellation; the draws are PatchList topology with the hull shader's input control point count)
  Handle geometry_shader = 0;   // not 0: a geometry shader (stage Geometry) that runs between the vertex and the pixel shader; needs kFeatureGeometryShaders, and the draws must make what it takes
  Handle compute_shader = 0;   // not 0: a compute pipeline of that shader (stage Compute), and nothing else of the description is used; needs kFeatureCompute
  uint32_t vertex_stride = 0;
  graphics::VertexAttribute attributes[graphics::kMaxVertexAttributes] = {};
  uint32_t attribute_count = 0;
  uint32_t varying_count = 0;
  uint32_t texture_count = 0;
  graphics::CullMode cull_mode = graphics::CullMode::Back;
  graphics::FrontFace front_face = graphics::FrontFace::CounterClockwise;
  bool depth_test = false, depth_write = false, alpha_blend = false;
  graphics::StencilState stencil;
  graphics::BlendState blend;                                  // needs the adapter's kFeatureBlendState (caps.features)
  graphics::CompareOp depth_compare = graphics::CompareOp::LessEqual;   // ... as does any comparison but LessEqual
  // (the attributes' per_instance flag needs kFeatureInstancing)
  bool independent_blend = false;    // needs kFeatureIndependentBlend: target n (1..3) blends with target_blend[n - 1], target 0 with `blend` (see graphics::GraphicsPipelineDesc)
  graphics::BlendState target_blend[graphics::kMaxColorTargets - 1];
  bool multisample = false;          // needs kFeatureMultisample: coverage per sample in a pass of multisampled images (see graphics::GraphicsPipelineDesc::multisample)
  uint32_t color_target_count = 1;   // the colour outputs the pixel shader fills (SV_Target0 ..); more than one needs the adapter's kFeatureMultipleTargets, and a pass with that many targets
};

// --- Presenting -----------------------------------------------------------------------

struct PresentArgs {
  Handle source = 0;             // the back buffer (kAllocPresent unless `destination` is given)
  Handle destination = 0;        // 0: flip - the source itself is shown. Otherwise the region is copied into it and it is shown (a blit present).
  graphics::Rect2D source_rect;  // width 0: the whole source
  int32_t dest_x = 0, dest_y = 0;
};

struct DisplayModeInfo {
  uint32_t width = 0, height = 0;
};

}  // namespace dxgk
