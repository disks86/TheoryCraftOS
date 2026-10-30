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

// The Direct3D 12 binding model of the kernel half: root signatures (parsed from the serialized form D3D12SerializeRootSignature writes, or from the RTS0 chunk of a DXBC container), descriptor
// heaps and the descriptors in them, and resource states. The command stream ops that use them are in CommandStream.hpp; the stream runner (Adapter.cpp) lowers them, at each draw and dispatch,
// to the texture, storage buffer and constant bindings the devices have: t<n> is texture unit n, u<n> storage slot n, and b0 the push constants (the first constant buffer, read by the CPU).
#pragma once
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include "Kernel/Dxgkrnl/VidMm.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/SharedPointer.hpp"

namespace dxgk {

// --- Resource states (D3D12_RESOURCE_STATES) ------------------------------------------------------------------------------------------------------------------------

namespace state {
constexpr uint32_t kCommon = 0, kPresent = 0;
constexpr uint32_t kVertexAndConstantBuffer = 0x1, kIndexBuffer = 0x2, kRenderTarget = 0x4, kUnorderedAccess = 0x8, kDepthWrite = 0x10, kDepthRead = 0x20, kNonPixelShaderResource = 0x40,
                   kPixelShaderResource = 0x80, kStreamOut = 0x100, kIndirectArgument = 0x200, kCopyDest = 0x400, kCopySource = 0x800, kResolveDest = 0x1000, kResolveSource = 0x2000;
constexpr uint32_t kGenericRead = kVertexAndConstantBuffer | kIndexBuffer | kNonPixelShaderResource | kPixelShaderResource | kIndirectArgument | kCopySource;
constexpr uint32_t kAll = kVertexAndConstantBuffer | kIndexBuffer | kRenderTarget | kUnorderedAccess | kDepthWrite | kDepthRead | kNonPixelShaderResource | kPixelShaderResource | kStreamOut |
                          kIndirectArgument | kCopyDest | kCopySource | kResolveDest | kResolveSource;
// The states that write: one of them cannot be combined with another.
constexpr uint32_t kWrite = kRenderTarget | kUnorderedAccess | kDepthWrite | kStreamOut | kCopyDest | kResolveDest;
// What an allocation in the common state may be used for without a barrier (D3D12 promotes it implicitly): reads and copies.
constexpr uint32_t kPromotable = kVertexAndConstantBuffer | kIndexBuffer | kNonPixelShaderResource | kPixelShaderResource | kCopyDest | kCopySource;
}  // namespace state
constexpr uint32_t kStateMask = state::kAll;

// Whether using an allocation in `needed` (any one of the bits) is allowed while it is in `current`.
inline bool state_allows(uint32_t current, uint32_t needed) {
  if (current == state::kCommon) return (needed & ~state::kPromotable) == 0;
  return (current & needed) != 0;
}

// --- Root signatures ------------------------------------------------------------------------------------------------------------------------------------------------

enum class RootParameterType : uint8_t { Table, Constants, Cbv, Srv, Uav };
enum class RangeType : uint8_t { Srv, Uav, Cbv, Sampler };

constexpr uint32_t kMaxRootParameters = 16, kMaxTableRanges = 8, kMaxStaticSamplers = 8, kMaxRootDwords = 64, kUnboundedDescriptors = 0xFFFFFFFFu, kMaxHeapDescriptors = 4096;
constexpr uint32_t kMaxRootSignatureBytes = 4096;

struct DescriptorRange {
  RangeType type = RangeType::Srv;
  uint32_t count = 0;            // kUnboundedDescriptors: to the end of the heap
  uint32_t base_register = 0;
  uint32_t space = 0;
  uint32_t offset = 0;           // in the table, resolved (an "append" is the end of the range before)
};

struct RootParameter {
  RootParameterType type = RootParameterType::Constants;
  uint32_t visibility = 0;       // D3D12_SHADER_VISIBILITY: 0 all, 1 vertex, 2 hull, 3 domain, 4 geometry, 5 pixel
  uint32_t shader_register = 0, space = 0;
  uint32_t constant_count = 0;   // Constants: 32-bit values
  uint32_t range_count = 0;      // Table
  DescriptorRange ranges[kMaxTableRanges];
  uint32_t table_size = 0;       // Table: descriptors up to the end of the last range (kUnboundedDescriptors if the last is unbounded)
  bool sampler_table = false;    // Table: of the sampler heap
};

struct StaticSampler {
  graphics::SamplerDesc sampler;
  uint32_t shader_register = 0, space = 0, visibility = 0;
};

struct RootSignatureDesc {
  RootParameter parameters[kMaxRootParameters];
  uint32_t parameter_count = 0;
  StaticSampler samplers[kMaxStaticSamplers];
  uint32_t sampler_count = 0;
  uint32_t flags = 0;
  uint32_t dwords = 0;           // the cost in the 64 a root signature has: a table 1, constants their number, a root descriptor 2
};

// Parses a serialized root signature (version 1.0 or 1.1), or a DXBC container with an RTS0 chunk. kInvalidParameter for anything that is not one, truncated or against the rules (too many
// parameters or DWORDs, a table of both samplers and views, an unbounded range that is not the last, overlapping ranges).
NtStatus parse_root_signature(ConstByteView blob, RootSignatureDesc* out);

class RootSignatureObject {
 public:
  RootSignatureDesc desc;
};

// --- Descriptor heaps -----------------------------------------------------------------------------------------------------------------------------------------------

enum class DescriptorHeapType : uint8_t { CbvSrvUav, Sampler };
enum class DescriptorKind : uint8_t { None, Srv, Uav, Cbv, Sampler };

// What a descriptor was made to say (CreateShaderResourceView and friends): a texture (Srv), a storage buffer range (Uav), a constant buffer range (Cbv) or a sampler.
struct DescriptorDesc {
  DescriptorKind kind = DescriptorKind::None;
  Handle allocation = 0;
  uint64_t offset = 0, size = 0;   // buffers: the range (size 0: to the end)
  graphics::SamplerDesc sampler;
};

struct Descriptor {
  DescriptorKind kind = DescriptorKind::None;
  SharedPointer<Allocation> allocation;
  uint64_t offset = 0, size = 0;
  graphics::SamplerDesc sampler;
};

class DescriptorHeapObject {
 public:
  DescriptorHeapObject(allocators::IAllocator& allocator, DescriptorHeapType type) : type_(type), descriptors_(allocator) {}
  DescriptorHeapType type() const { return type_; }
  uint64_t count() const { return descriptors_.count(); }
  bool resize(uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
      if (!descriptors_.push_back(Descriptor{})) return false;
    return true;
  }
  const Descriptor& at(uint64_t index) const { return descriptors_[index]; }
  Descriptor& at(uint64_t index) { return descriptors_[index]; }

 private:
  DescriptorHeapType type_;
  List<Descriptor> descriptors_;
};

}  // namespace dxgk
