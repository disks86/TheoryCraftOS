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

// The Venus device's shader modules and pipelines (VenusDevice.hpp): TGSI compiled to SPIR-V per variant, the Vulkan pipeline state of a GraphicsPipelineDesc,
// the descriptor set layout of what the shaders use.
#include <new>
#include "Graphics/Graphics3D/Venus/VenusObjects.hpp"

namespace graphics {

// ---- shader modules -------------------------------------------------------------------------------------------------------------------------------
bool VenusShaderModule::create() {
  if (!desc_.tgsi) return false;
  const StringView text(desc_.tgsi);
  if (!text_.append(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()))) return false;   // (a string's bytes)
  desc_.tgsi = nullptr;   // (the caller's text may go; ours is text_)
  // A text with marks is read per variant (the marks become numbers and formats then); any other is read now, so that a module the device cannot run is refused
  // here, as the host refuses it on virgl.
  if (tgsi::has_marks(this->text())) return true;
  tgsi::ParseError error;
  if (!tgsi::parse(this->text(), &program_, &error)) {
    VenusDevice::trace("TGSI refused", error.line, error.message.view());
    return false;
  }
  return true;
}

IShaderModule* VenusDevice::create_shader_module(const ShaderModuleDesc& desc) {
  // (rasterizer ordered views: a pixel shader's, where the host has the pixel interlock)
  if (desc.rasterizer_ordered && (desc.stage != ShaderStage::Fragment || !(info().features3 & kFeature3RasterizerOrderedViews))) return nullptr;
  void* memory = allocator_.allocate_aligned(sizeof(VenusShaderModule), alignof(VenusShaderModule));
  if (!memory) return nullptr;
  VenusShaderModule* m = new (memory) VenusShaderModule(allocator_, desc);
  if (!m->create()) {
    m->~VenusShaderModule();
    allocator_.deallocate(memory);
    return nullptr;
  }
  return m;
}

void VenusDevice::destroy_shader_module(IShaderModule* module) {
  if (!module) return;
  VenusShaderModule* m = static_cast<VenusShaderModule*>(module);
  m->~VenusShaderModule();
  allocator_.deallocate(m);
}

// ---- pipelines ------------------------------------------------------------------------------------------------------------------------------------
IPipeline* VenusDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
  // (VirglDevice's refusals: what the kernel checks against the device's limits again here)
  const DeviceLimits l = info().limits;
  if (desc.texture_count > kMaxTextureUnits || !desc.vertex_shader || !desc.fragment_shader || desc.attribute_count > kMaxVertexAttributes || desc.varying_count > l.varyings) return nullptr;
  // (no forced sample count: Vulkan rasterizes with the attachments' samples, and the hosts' framebufferNoAttachmentsSampleCounts stop below 16 - kFeature3ForcedSampleCount is not offered)
  if (desc.forced_samples) return nullptr;
  if (desc.vertex_shader->stage() != ShaderStage::Vertex || desc.fragment_shader->stage() != ShaderStage::Fragment) return nullptr;
  if (desc.geometry_shader && desc.geometry_shader->stage() != ShaderStage::Geometry) return nullptr;
  if (!desc.hull_shader != !desc.domain_shader) return nullptr;
  if (desc.hull_shader && (desc.hull_shader->stage() != ShaderStage::Hull || desc.domain_shader->stage() != ShaderStage::Domain)) return nullptr;
  for (uint32_t i = 0; i < desc.attribute_count; ++i) {
    if (desc.attributes[i].location >= kMaxVertexAttributes || desc.attributes[i].slot >= kMaxVertexBuffers) return nullptr;
    for (uint32_t j = 0; j < i; ++j)
      if (desc.attributes[j].location == desc.attributes[i].location) return nullptr;
  }
  void* memory = allocator_.allocate_aligned(sizeof(VenusPipeline), alignof(VenusPipeline));
  if (!memory) return nullptr;
  VenusPipeline* p = new (memory) VenusPipeline(*this, desc);
  // The variant of the usual shape is made now (B8G8R8A8 target, nothing integer bound): a pipeline whose shaders the device cannot run is refused here.
  VenusVariantKey key;
  key.color_format[0] = VK_FORMAT_B8G8R8A8_UNORM;
  for (uint32_t t = 1; t < desc.color_target_count && t < kMaxColorTargets; ++t) key.color_format[t] = VK_FORMAT_B8G8R8A8_UNORM;
  key.depth_format = desc.depth_test || desc.depth_write || desc.stencil.enable ? VK_FORMAT_D32_SFLOAT : 0;
  key.topology = static_cast<uint32_t>(desc.hull_shader ? PrimitiveTopology::PatchList : PrimitiveTopology::TriangleList);
  key.patch_points = desc.hull_shader ? static_cast<const VenusShaderModule*>(desc.hull_shader)->desc().tess_input_points : 0;
  if (!p->variant(key)) {
    p->~VenusPipeline();
    allocator_.deallocate(memory);
    return nullptr;
  }
  return p;
}

IPipeline* VenusDevice::create_compute_pipeline(const IShaderModule* shader) {
  if (!shader || shader->stage() != ShaderStage::Compute) return nullptr;
  void* memory = allocator_.allocate_aligned(sizeof(VenusPipeline), alignof(VenusPipeline));
  if (!memory) return nullptr;
  VenusPipeline* p = new (memory) VenusPipeline(*this, static_cast<const VenusShaderModule*>(shader));
  if (!p->variant(VenusVariantKey())) {
    p->~VenusPipeline();
    allocator_.deallocate(memory);
    return nullptr;
  }
  return p;
}

void VenusDevice::destroy_pipeline(IPipeline* pipeline) {
  if (!pipeline) return;
  VenusPipeline* p = static_cast<VenusPipeline*>(pipeline);
  p->~VenusPipeline();
  allocator_.deallocate(p);
}

VenusPipeline::~VenusPipeline() {
  const TcosKernelVulkan& vk = device_.vk();
  for (const Set& s : sets_) vk.FreeDescriptorSets(device_.device(), s.pool, 1, &s.set);
  for (Variant& v : variants_) {
    if (v.pipeline) vk.DestroyPipeline(device_.device(), v.pipeline, nullptr);
    if (v.layout) vk.DestroyPipelineLayout(device_.device(), v.layout, nullptr);
    if (v.set_layout) vk.DestroyDescriptorSetLayout(device_.device(), v.set_layout, nullptr);
  }
}

VenusPipeline::Variant* VenusPipeline::variant(const VenusVariantKey& key) {
  for (Variant& v : variants_)
    if (v.key == key) return v.pipeline ? &v : nullptr;
  Variant* v = variants_.emplace();
  if (!v) return nullptr;
  v->key = key;
  if (!make(*v)) {
    // (kept without a pipeline: the same shape is not tried again)
    return nullptr;
  }
  return v;
}

VkDescriptorSet VenusPipeline::next_set(Variant& v) {
  uint32_t seen = 0;
  for (const Set& s : sets_)
    if (s.variant == &v && seen++ == v.sets_used) {
      ++v.sets_used;
      return s.set;
    }
  // (another set of the layout: from the device's newest pool, or a new pool)
  VkDescriptorPool pool = device_.descriptor_pool_current();
  VkDescriptorSet set = nullptr;
  VkDescriptorSetAllocateInfo ai = vk_struct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
  ai.descriptorSetCount = 1, ai.pSetLayouts = &v.set_layout;
  for (uint32_t attempt = 0; attempt < 2 && !set; ++attempt) {
    if (!pool) pool = device_.descriptor_pool();
    if (!pool) return nullptr;
    ai.descriptorPool = pool;
    if (device_.vk().AllocateDescriptorSets(device_.device(), &ai, &set) != VK_SUCCESS) set = nullptr, pool = nullptr;
  }
  if (!set) return nullptr;
  Set s;
  s.variant = &v, s.set = set, s.pool = pool;
  if (!sets_.push_back(s)) {
    device_.vk().FreeDescriptorSets(device_.device(), pool, 1, &set);
    return nullptr;
  }
  ++v.sets_used;
  return set;
}

namespace {

struct VertexFormatInfo {
  VkFormat format;
  bool one_w;   // (read through a format with more components: the fourth is 1)
};

VkFormat vertex_vk_format(VertexFormat f) {
  using F = VertexFormat;
  switch (f) {
    case F::Float1: return VK_FORMAT_R32_SFLOAT;
    case F::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case F::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case F::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case F::Unorm8x1: return VK_FORMAT_R8_UNORM;
    case F::Unorm8x2: return VK_FORMAT_R8G8_UNORM;
    case F::Unorm8x3: return VK_FORMAT_R8G8B8_UNORM;
    case F::Unorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::Snorm8x1: return VK_FORMAT_R8_SNORM;
    case F::Snorm8x2: return VK_FORMAT_R8G8_SNORM;
    case F::Snorm8x3: return VK_FORMAT_R8G8B8_SNORM;
    case F::Snorm8x4: return VK_FORMAT_R8G8B8A8_SNORM;
    case F::Uint8x1: return VK_FORMAT_R8_UINT;
    case F::Uint8x2: return VK_FORMAT_R8G8_UINT;
    case F::Uint8x3: return VK_FORMAT_R8G8B8_UINT;
    case F::Uint8x4: return VK_FORMAT_R8G8B8A8_UINT;
    case F::Sint8x1: return VK_FORMAT_R8_SINT;
    case F::Sint8x2: return VK_FORMAT_R8G8_SINT;
    case F::Sint8x3: return VK_FORMAT_R8G8B8_SINT;
    case F::Sint8x4: return VK_FORMAT_R8G8B8A8_SINT;
    case F::Unorm16x1: return VK_FORMAT_R16_UNORM;
    case F::Unorm16x2: return VK_FORMAT_R16G16_UNORM;
    case F::Unorm16x3: return VK_FORMAT_R16G16B16_UNORM;
    case F::Unorm16x4: return VK_FORMAT_R16G16B16A16_UNORM;
    case F::Snorm16x1: return VK_FORMAT_R16_SNORM;
    case F::Snorm16x2: return VK_FORMAT_R16G16_SNORM;
    case F::Snorm16x3: return VK_FORMAT_R16G16B16_SNORM;
    case F::Snorm16x4: return VK_FORMAT_R16G16B16A16_SNORM;
    case F::Uint16x1: return VK_FORMAT_R16_UINT;
    case F::Uint16x2: return VK_FORMAT_R16G16_UINT;
    case F::Uint16x3: return VK_FORMAT_R16G16B16_UINT;
    case F::Uint16x4: return VK_FORMAT_R16G16B16A16_UINT;
    case F::Sint16x1: return VK_FORMAT_R16_SINT;
    case F::Sint16x2: return VK_FORMAT_R16G16_SINT;
    case F::Sint16x3: return VK_FORMAT_R16G16B16_SINT;
    case F::Sint16x4: return VK_FORMAT_R16G16B16A16_SINT;
    case F::Half1: return VK_FORMAT_R16_SFLOAT;
    case F::Half2: return VK_FORMAT_R16G16_SFLOAT;
    case F::Half3: return VK_FORMAT_R16G16B16_SFLOAT;
    case F::Half4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::Uint32x1: return VK_FORMAT_R32_UINT;
    case F::Uint32x2: return VK_FORMAT_R32G32_UINT;
    case F::Uint32x3: return VK_FORMAT_R32G32B32_UINT;
    case F::Uint32x4: return VK_FORMAT_R32G32B32A32_UINT;
    case F::Sint32x1: return VK_FORMAT_R32_SINT;
    case F::Sint32x2: return VK_FORMAT_R32G32_SINT;
    case F::Sint32x3: return VK_FORMAT_R32G32B32_SINT;
    case F::Sint32x4: return VK_FORMAT_R32G32B32A32_SINT;
    case F::Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case F::Rgb10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::Rgb10A2Uint: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case F::Rg11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case F::Bgrx8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

// The four-component format of the same type a three-component one is read through when the host cannot read it as it is (the fourth then reads as 1).
VkFormat widened(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

VkCompareOp compare_op(CompareOp c) { return static_cast<VkCompareOp>(static_cast<uint32_t>(c)); }   // (the same order: Never .. Always)
VkStencilOp stencil_op(StencilOp s) { return static_cast<VkStencilOp>(static_cast<uint32_t>(s)); }   // (Keep, Zero, Replace, IncrClamp, DecrClamp, Invert, IncrWrap, DecrWrap)

VkBlendFactor blend_factor(BlendFactor f) {
  switch (f) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InvDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::SrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case BlendFactor::ConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::InvConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::ConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case BlendFactor::InvConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case BlendFactor::Src1Color: return VK_BLEND_FACTOR_SRC1_COLOR;
    case BlendFactor::InvSrc1Color: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case BlendFactor::Src1Alpha: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case BlendFactor::InvSrc1Alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp blend_op(BlendOp o) {
  switch (o) {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min: return VK_BLEND_OP_MIN;
    case BlendOp::Max: return VK_BLEND_OP_MAX;
  }
  return VK_BLEND_OP_ADD;
}

VkLogicOp logic_op(LogicOp o) {
  switch (o) {
    case LogicOp::Clear: return VK_LOGIC_OP_CLEAR;
    case LogicOp::Set: return VK_LOGIC_OP_SET;
    case LogicOp::Copy: return VK_LOGIC_OP_COPY;
    case LogicOp::CopyInverted: return VK_LOGIC_OP_COPY_INVERTED;
    case LogicOp::Noop: return VK_LOGIC_OP_NO_OP;
    case LogicOp::Invert: return VK_LOGIC_OP_INVERT;
    case LogicOp::And: return VK_LOGIC_OP_AND;
    case LogicOp::Nand: return VK_LOGIC_OP_NAND;
    case LogicOp::Or: return VK_LOGIC_OP_OR;
    case LogicOp::Nor: return VK_LOGIC_OP_NOR;
    case LogicOp::Xor: return VK_LOGIC_OP_XOR;
    case LogicOp::Equiv: return VK_LOGIC_OP_EQUIVALENT;
    case LogicOp::AndReverse: return VK_LOGIC_OP_AND_REVERSE;
    case LogicOp::AndInverted: return VK_LOGIC_OP_AND_INVERTED;
    case LogicOp::OrReverse: return VK_LOGIC_OP_OR_REVERSE;
    case LogicOp::OrInverted: return VK_LOGIC_OP_OR_INVERTED;
  }
  return VK_LOGIC_OP_COPY;
}

// (one face's state; whether the stencil test runs at all is the front state's `enable`, for both faces)
VkStencilOpState stencil_state(const StencilState& s, bool enabled) {
  VkStencilOpState o{};
  o.failOp = stencil_op(s.fail_op), o.passOp = stencil_op(s.pass_op), o.depthFailOp = stencil_op(s.depth_fail_op);
  o.compareOp = enabled ? compare_op(s.compare) : VK_COMPARE_OP_ALWAYS;
  o.compareMask = s.compare_mask, o.writeMask = enabled ? s.write_mask : 0, o.reference = s.reference;
  return o;
}

tgsi::ValueType format_value_type(uint32_t vk_format) {
  switch (vk_format) {
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
      return tgsi::ValueType::Uint;
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT:
      return tgsi::ValueType::Sint;
    default: return tgsi::ValueType::Float;
  }
}

// Whether the text has an atomic operation on IMAGE[k] (a line with ATOM and that register).
bool has_image_atomics(StringView text, uint32_t k) {
  char name[16] = "IMAGE[";
  uint32_t n = 6;
  char digits[4];
  uint32_t d = 0;
  do digits[d++] = static_cast<char>('0' + k % 10), k /= 10;
  while (k && d < 4);
  while (d) name[n++] = digits[--d];
  name[n++] = ']';
  const StringView reg(name, n);
  uint64_t start = 0;
  while (start < text.size()) {
    uint64_t end = start;
    while (end < text.size() && text[end] != '\n') ++end;
    const StringView line = text.substr(start, end - start);
    bool atom = false, uses = false;
    for (uint64_t i = 0; i + 4 <= line.size(); ++i)
      if (line.substr(i, 4) == StringView("ATOM")) atom = true;
    for (uint64_t i = 0; i + reg.size() <= line.size(); ++i)
      if (line.substr(i, reg.size()) == reg) uses = true;
    if (atom && uses) return true;
    start = end + 1;
  }
  return false;
}

// The register a stream output entry names (VirglDevice's rule: output 0 is the position as the shader wrote it, kept after the varyings for each stream).
uint32_t stream_register(const StreamOutputEntry& e, uint32_t varyings) { return e.output == 0 ? varyings + 1 + e.stream : e.output; }

}  // namespace

// Compiles each stage's TGSI for the variant, then makes the layout and the pipeline.
uint32_t VenusPipeline::stage_modules(const VenusShaderModule** modules) const {
  uint32_t count = 0;
  if (compute_) {
    modules[count++] = compute_;
    return count;
  }
  modules[count++] = static_cast<const VenusShaderModule*>(desc_.vertex_shader);
  if (desc_.hull_shader) {
    modules[count++] = static_cast<const VenusShaderModule*>(desc_.hull_shader);
    modules[count++] = static_cast<const VenusShaderModule*>(desc_.domain_shader);
  }
  if (desc_.geometry_shader) modules[count++] = static_cast<const VenusShaderModule*>(desc_.geometry_shader);
  modules[count++] = static_cast<const VenusShaderModule*>(desc_.fragment_shader);
  return count;
}

bool VenusPipeline::make(Variant& v) {
  const TcosKernelVulkan& vk = device_.vk();
  VenusContext& ctx = device_.context();
  const DeviceInfo info = device_.info();
  // The stages, in pipeline order.
  const VenusShaderModule* modules[5] = {};
  VkShaderStageFlagBits stage_bits[5] = {};
  const uint32_t count = stage_modules(modules);
  for (uint32_t s = 0; s < count; ++s) {
    const ShaderStage st = modules[s]->stage();
    stage_bits[s] = st == ShaderStage::Compute ? VK_SHADER_STAGE_COMPUTE_BIT : st == ShaderStage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT
                    : st == ShaderStage::Hull ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : st == ShaderStage::Domain ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                    : st == ShaderStage::Geometry ? VK_SHADER_STAGE_GEOMETRY_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    v.stage_module[s] = modules[s];
  }
  v.stage_count = count;
  const uint32_t last_vertex_stage = compute_ ? 0 : count - 2;   // (the stage before the fragment shader)
  // stream output entries of the last stage before the rasterizer
  tgsi::StreamOutputEntry so[kMaxStreamOutputEntries];
  uint32_t so_count = 0;
  uint32_t swap = 0;
  if (!compute_ && desc_.stream_output_count) {
    if (!ctx.caps().transform_feedback) return false;
    uint32_t fill[kMaxStreamOutputBuffers] = {};
    for (uint32_t e = 0; e < desc_.stream_output_count && e < kMaxStreamOutputEntries; ++e) {
      const StreamOutputEntry& entry = desc_.stream_output[e];
      if (entry.buffer >= kMaxStreamOutputBuffers) return false;
      so[so_count].reg = stream_register(entry, info.limits.varyings);
      so[so_count].first = entry.start_component, so[so_count].count = entry.component_count;
      so[so_count].buffer = entry.buffer, so[so_count].stream = entry.stream;
      so[so_count].offset = fill[entry.buffer];
      fill[entry.buffer] += entry.component_count;
      ++so_count;
    }
    if (desc_.rasterized_stream > 0) swap = static_cast<uint32_t>(desc_.rasterized_stream);
  }
  // the patch outputs' locations: after the per-vertex ones the boundary has
  uint32_t patch_base = 0;
  for (uint32_t s = 0; s < count; ++s) {
    if (stage_bits[s] != VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT && stage_bits[s] != VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) continue;
    for (const tgsi::Declaration& d : modules[s]->program().declarations)
      if (d.two_dimensional && d.semantic == tgsi::Semantic::Generic && d.semantic_index + (d.last - d.first) + 1 > patch_base) patch_base = d.semantic_index + (d.last - d.first) + 1;
  }
  VkShaderModule shader_modules[5] = {};
  VkPipelineShaderStageCreateInfo stages[5] = {};
  bool ok = true;
  // the union of what the stages use
  for (uint32_t s = 0; s < count && ok; ++s) {
    const VenusShaderModule& m = *modules[s];
    tgsi::CompileOptions o;
    o.constant_rows = kVenusConstantRows;
    o.system_row = kVenusSystemRow;
    o.stage_slot = s;
    for (uint32_t u = 0; u < tgsi::kMaxUnits; ++u) o.sview_type[u] = static_cast<tgsi::ValueType>(v.key.unit_type[u]);
    for (uint32_t k = 0; k < tgsi::kMaxTexelViews; ++k) o.sview_type[tgsi::kMaxUnits + k] = static_cast<tgsi::ValueType>(v.key.texel_type[s][k]);
    // (an image nothing is bound to - at the pipeline's creation, or a slot left empty - is given a format its accesses take: R32_UINT where it has atomics, which
    // only 32-bit integer images do; four floats otherwise)
    tgsi::ImageFormat image_formats[tgsi::kMaxImages];
    for (uint32_t k = 0; k < tgsi::kMaxImages; ++k) {
      image_formats[k] = static_cast<tgsi::ImageFormat>(v.key.image_format[s][k]);
      if (image_formats[k] == tgsi::ImageFormat::None) image_formats[k] = has_image_atomics(m.text(), k) ? tgsi::ImageFormat::R32Uint : tgsi::ImageFormat::R32G32B32A32Float;
      o.image_format[k] = image_formats[k];
      o.image_type[k] = tgsi::ValueType::Float;
    }
    if (!compute_) {
      for (uint32_t i = 0; i < desc_.attribute_count; ++i) {
        const VertexAttribute& a = desc_.attributes[i];
        const VertexType t = vertex_format_type(a.format);
        o.attribute_type[a.location] = !vertex_format_is_integer(a.format) ? tgsi::ValueType::Float
                                       : (t == VertexType::Sint8 || t == VertexType::Sint16 || t == VertexType::Sint32) ? tgsi::ValueType::Sint : tgsi::ValueType::Uint;
        const VkFormat f = vertex_vk_format(a.format);
        VkFormatProperties fp{};
        vk.GetPhysicalDeviceFormatProperties(ctx.physical(), f, &fp);
        if (a.format == VertexFormat::Bgrx8Unorm || (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) && widened(f) != VK_FORMAT_UNDEFINED))
          o.attribute_one_w |= static_cast<uint16_t>(1u << a.location);
      }
      for (uint32_t t = 0; t < kMaxColorTargets; ++t) o.color_type[t] = format_value_type(v.key.color_format[t]);
      o.clip_distances = desc_.clip_distances && ctx.caps().shader_clip_distance ? 8 : 0;
      o.clip_mask = desc_.clip_distances;
      o.dual_source = desc_.blend.uses_dual_source();
      o.color_targets = desc_.color_target_count ? desc_.color_target_count : 1;
      o.patch_base = patch_base;
      o.output_layer_any_stage = ctx.caps().shader_output_layer && ctx.caps().shader_output_viewport_index;
      const PrimitiveTopology topology = static_cast<PrimitiveTopology>(v.key.topology);
      // (Vulkan wants a point size from the last stage when it draws points; the producers never write one)
      if (s == last_vertex_stage) {
        const bool gs_points = desc_.geometry_shader && static_cast<const VenusShaderModule*>(desc_.geometry_shader)->desc().geometry_output == GeometryTopology::PointList;
        const bool tes_points = desc_.domain_shader && static_cast<const VenusShaderModule*>(desc_.domain_shader)->desc().tess_output == TessOutput::Point;
        o.write_point_size = desc_.geometry_shader ? gs_points : desc_.domain_shader ? tes_points : topology == PrimitiveTopology::PointList;
        o.view_offsets = (v.key.flags & kVariantViewOffsets) != 0;
        o.stream_output = so_count ? so : nullptr;
        o.stream_output_count = so_count;
        for (uint32_t b = 0; b < kMaxStreamOutputBuffers; ++b) o.stream_output_stride[b] = desc_.stream_output_stride[b] / 4;
      }
      if (stage_bits[s] == VK_SHADER_STAGE_GEOMETRY_BIT) o.swap_stream = swap;
      if (stage_bits[s] == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {   // (the tessellator's settings are the hull's, as VirglDevice writes them over the domain's)
        const ShaderModuleDesc& h = static_cast<const VenusShaderModule*>(desc_.hull_shader)->desc();
        o.tess_override = true;
        o.tess_cw = h.tess_output == TessOutput::TriangleCw;
        o.tess_points = h.tess_output == TessOutput::Point;
        o.tess_spacing = h.tess_partitioning == TessPartitioning::FractionalOdd ? 0 : h.tess_partitioning == TessPartitioning::FractionalEven ? 1 : 2;
      }
    } else {
      o.shared_bytes = m.desc().shared_memory_bytes;
    }
    o.ddy_up = m.desc().tgsi_gl;
    o.pixel_interlock = m.desc().rasterizer_ordered;   // (a module with rasterizer ordered views: the device has the interlock, or it offers no kFeature3RasterizerOrderedViews)
    o.unnormalized_units = v.key.unnormalized;
    // The program: the module's, or (marks) its text read with the bound views' numbers.
    tgsi::Program marked(device_.allocator());
    const tgsi::Program* program = &m.program();
    if (tgsi::has_marks(m.text())) {
      tgsi::MarkValues mv;
      for (uint32_t k = 0; k < tgsi::kMarkImages; ++k) mv.image_format[k] = image_formats[k];
      for (uint32_t k = 0; k < tgsi::kMarkImages; ++k) mv.image_count[k] = 0x7FFFFFFF;   // (Vulkan's buffer views are exact: the shader's own bounds stay out of the way)
      for (uint32_t k = 0; k < tgsi::kMarkTexelViews; ++k) mv.texel_count[k] = 0x7FFFFFFF;
      ByteBuffer text(device_.allocator());
      tgsi::ParseError pe;
      if (!tgsi::substitute_marks(m.text(), mv, &text) || !tgsi::parse(StringView(text.view()), &marked, &pe)) {
        VenusDevice::trace("TGSI with marks refused", pe.line, pe.message.view());
        ok = false;
        break;
      }
      program = &marked;
    }
    tgsi::CompileResult r(device_.allocator());
    if (!tgsi::compile_spirv(*program, o, &r)) {
      VenusDevice::trace("TGSI not compiled", r.line, r.error.view());
      ok = false;
      break;
    }
    v.unit_mask |= r.sview_mask & 0xFFFFu, v.sampler_mask |= r.sampler_mask & 0xFFFFu;
    v.texel_mask[s] = r.sview_mask >> tgsi::kMaxUnits;
    v.storage_mask[s] = r.storage_mask, v.image_mask[s] = r.image_mask;
    v.constants |= r.constants;
    for (uint32_t k = 0; k < tgsi::kMaxImages; ++k)
      if (r.image_buffer[k]) v.image_buffer_mask[s] |= uint64_t{1} << k;
    VkShaderModuleCreateInfo si = vk_struct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    si.codeSize = r.words.count() * 4;
    si.pCode = r.words.view().data();
    if (vk.CreateShaderModule(device_.device(), &si, nullptr, &shader_modules[s]) != VK_SUCCESS) {
      shader_modules[s] = nullptr;
      ok = false;
      break;
    }
    stages[s] = vk_struct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    stages[s].stage = stage_bits[s];
    stages[s].module = shader_modules[s];
    stages[s].pName = "main";
  }
  // The descriptor set layout: what the stages use, every stage seeing all of it.
  if (ok) {
    static constexpr uint32_t kMaxBindings = 1 + 2 * tgsi::kMaxUnits + tgsi::kMaxStages * (tgsi::kMaxTexelViews + tgsi::kMaxStorage + tgsi::kMaxImages);
    VkDescriptorSetLayoutBinding bindings[kMaxBindings];
    uint32_t n = 0;
    const VkShaderStageFlags all = compute_ ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS;
    auto add = [&](uint32_t binding, VkDescriptorType type) { bindings[n++] = {binding, type, 1, all, nullptr}; };
    add(tgsi::kBindingConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    for (uint32_t u = 0; u < tgsi::kMaxUnits; ++u) {
      if (v.unit_mask >> u & 1) add(tgsi::kBindingTexture + u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
      if (v.sampler_mask >> u & 1) add(tgsi::kBindingSampler + u, VK_DESCRIPTOR_TYPE_SAMPLER);
    }
    for (uint32_t s = 0; s < count; ++s) {
      for (uint32_t k = 0; k < tgsi::kMaxTexelViews; ++k)
        if (v.texel_mask[s] >> k & 1) add(tgsi::binding_texel(s, k), VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
      for (uint32_t k = 0; k < tgsi::kMaxStorage; ++k)
        if (v.storage_mask[s] >> k & 1) add(tgsi::binding_storage(s, k), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
      for (uint32_t k = 0; k < tgsi::kMaxImages; ++k)
        if (v.image_mask[s] >> k & 1)
          add(tgsi::binding_image(s, k), (v.image_buffer_mask[s] >> k & 1) ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }
    VkDescriptorSetLayoutCreateInfo li = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    li.bindingCount = n, li.pBindings = bindings;
    ok = vk.CreateDescriptorSetLayout(device_.device(), &li, nullptr, &v.set_layout) == VK_SUCCESS;
    if (!ok) v.set_layout = nullptr;
    if (ok) {
      VkPipelineLayoutCreateInfo pi = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
      pi.setLayoutCount = 1, pi.pSetLayouts = &v.set_layout;
      ok = vk.CreatePipelineLayout(device_.device(), &pi, nullptr, &v.layout) == VK_SUCCESS;
      if (!ok) v.layout = nullptr;
    }
  }
  if (ok) {
    if (compute_) {
      VkComputePipelineCreateInfo ci = vk_struct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
      ci.stage = stages[0];
      ci.layout = v.layout;
      ok = vk.CreateComputePipelines(device_.device(), nullptr, 1, &ci, nullptr, &v.pipeline) == VK_SUCCESS;
      if (!ok) v.pipeline = nullptr;
    } else {
      ok = make_graphics(v, stages, count);
    }
  }
  for (VkShaderModule sm : shader_modules)
    if (sm) vk.DestroyShaderModule(device_.device(), sm, nullptr);   // (the pipeline keeps what it needs)
  return ok;
}

bool VenusPipeline::make_graphics(Variant& v, const VkPipelineShaderStageCreateInfo* stages, uint32_t stage_count) {
  const TcosKernelVulkan& vk = device_.vk();
  VenusContext& ctx = device_.context();
  const GraphicsPipelineDesc& d = desc_;
  const bool gl = static_cast<const VenusShaderModule*>(d.vertex_shader)->desc().tgsi_gl;
  // vertex input
  VkVertexInputBindingDescription bindings[kMaxVertexBuffers];
  VkVertexInputBindingDivisorDescriptionEXT divisors[kMaxVertexBuffers];
  VkVertexInputAttributeDescription attributes[kMaxVertexAttributes];
  uint32_t binding_count = 0, divisor_count = 0;
  // (each slot is a binding; an attribute that steps per instance at a rate of its own gets a binding of its own on the same buffer, as virgl's divisors are per binding)
  uint8_t binding_of[kMaxVertexAttributes] = {};
  for (uint32_t i = 0; i < d.attribute_count; ++i) {
    const VertexAttribute& a = d.attributes[i];
    const uint32_t stride = a.slot == 0 ? d.vertex_stride : d.slot_strides[a.slot];
    const uint32_t rate = a.per_instance ? a.step_rate : 0xFFFFFFFFu;
    int32_t found = -1;
    for (uint32_t b = 0; b < binding_count; ++b) {
      const bool per_instance = bindings[b].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
      uint32_t brate = 0xFFFFFFFFu;
      if (per_instance) {
        brate = 1;
        for (uint32_t k = 0; k < divisor_count; ++k)
          if (divisors[k].binding == b) brate = divisors[k].divisor;
      }
      if (binding_slot_[b] == a.slot && per_instance == a.per_instance && brate == rate) found = static_cast<int32_t>(b);
    }
    if (found < 0) {
      if (binding_count >= kMaxVertexBuffers) return false;
      found = static_cast<int32_t>(binding_count);
      bindings[binding_count] = {binding_count, stride, a.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
      binding_slot_[binding_count] = a.slot;
      if (a.per_instance && a.step_rate != 1) {
        if (!ctx.caps().vertex_attribute_divisor || (a.step_rate == 0 && !ctx.caps().vertex_attribute_zero_divisor)) return false;
        divisors[divisor_count++] = {binding_count, a.step_rate};
      }
      ++binding_count;
    }
    binding_of[i] = static_cast<uint8_t>(found);
    VkFormat f = vertex_vk_format(a.format);
    VkFormatProperties fp{};
    vk.GetPhysicalDeviceFormatProperties(ctx.physical(), f, &fp);
    if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
      f = widened(f);   // (the shader sees the fourth component as 1: CompileOptions::attribute_one_w)
      if (f == VK_FORMAT_UNDEFINED) return false;
    }
    attributes[i] = {a.location, binding_of[i], f, a.offset};
  }
  binding_count_ = binding_count;
  VkPipelineVertexInputDivisorStateCreateInfoEXT div = vk_struct<VkPipelineVertexInputDivisorStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);
  div.vertexBindingDivisorCount = divisor_count, div.pVertexBindingDivisors = divisors;
  VkPipelineVertexInputStateCreateInfo vi = vk_struct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  vi.vertexBindingDescriptionCount = binding_count, vi.pVertexBindingDescriptions = bindings;
  vi.vertexAttributeDescriptionCount = d.attribute_count, vi.pVertexAttributeDescriptions = attributes;
  if (divisor_count) vi.pNext = &div;
  // input assembly
  VkPipelineInputAssemblyStateCreateInfo ia = vk_struct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  switch (static_cast<PrimitiveTopology>(v.key.topology)) {
    case PrimitiveTopology::TriangleList: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case PrimitiveTopology::LineList: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case PrimitiveTopology::PointList: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case PrimitiveTopology::PatchList: ia.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
    case PrimitiveTopology::LineListAdjacency: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; break;
    case PrimitiveTopology::TriangleListAdjacency: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; break;
  }
  VkPipelineTessellationDomainOriginStateCreateInfo origin = vk_struct<VkPipelineTessellationDomainOriginStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO);
  origin.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;   // (TGSI's tessellator is GL's)
  VkPipelineTessellationStateCreateInfo ts = vk_struct<VkPipelineTessellationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO);
  ts.pNext = &origin;
  ts.patchControlPoints = v.key.patch_points ? v.key.patch_points : 1;
  // viewports: all sixteen when the device has several (the ones past those set repeat viewport 0, as on virgl)
  VkPipelineViewportStateCreateInfo vp = vk_struct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  vp.viewportCount = vp.scissorCount = ctx.caps().multi_viewport ? kMaxViewports : 1;
  // rasterizer
  VkPipelineRasterizationStateCreateInfo rs = vk_struct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  rs.polygonMode = d.fill_mode == FillMode::Wireframe && ctx.caps().fill_mode_non_solid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
  rs.cullMode = d.cull_mode == CullMode::Back ? VK_CULL_MODE_BACK_BIT : d.cull_mode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
  rs.frontFace = d.front_face == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;   // (both as seen on screen: Vulkan's framebuffer is +Y down)
  rs.lineWidth = 1.0f;
  rs.rasterizerDiscardEnable = d.stream_output_count && d.rasterized_stream < 0 ? VK_TRUE : VK_FALSE;
  VkDepthBiasRepresentationInfoEXT bias_rep = vk_struct<VkDepthBiasRepresentationInfoEXT>(VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT);
  const void* rs_next = nullptr;
  if (d.depth_bias.enabled()) {
    rs.depthBiasEnable = VK_TRUE;
    rs.depthBiasConstantFactor = d.depth_bias.constant;
    rs.depthBiasSlopeFactor = d.depth_bias.slope;
    rs.depthBiasClamp = d.depth_bias.clamp;
    if (d.depth_bias.unorm_bits) {
      // (the depth buffer is D32F whatever Direct3D's format was: a normalised format's unit, 1 / (2^n - 1), is given as a value of its own)
      if (!ctx.caps().depth_bias_float) return false;
      bias_rep.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT;
      bias_rep.depthBiasExact = VK_TRUE;
      rs.depthBiasConstantFactor = d.depth_bias.constant / static_cast<float>((1u << d.depth_bias.unorm_bits) - 1u);
      rs_next = &bias_rep;
    }
  }
  VkPipelineRasterizationDepthClipStateCreateInfoEXT clip = vk_struct<VkPipelineRasterizationDepthClipStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT);
  if (!d.depth_clip) {
    if (!ctx.caps().depth_clamp) return false;
    rs.depthClampEnable = VK_TRUE;
    if (ctx.caps().depth_clip_enable) {
      clip.depthClipEnable = VK_FALSE;
      clip.pNext = rs_next;
      rs_next = &clip;
    }
  }
  VkPipelineRasterizationConservativeStateCreateInfoEXT cons = vk_struct<VkPipelineRasterizationConservativeStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT);
  if (d.conservative) {
    if (!ctx.caps().conservative_rasterization) return false;
    cons.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    cons.pNext = rs_next;
    rs_next = &cons;
  }
  VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoke = vk_struct<VkPipelineRasterizationProvokingVertexStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT);
  if (gl) {   // (GL's flat varyings come from the last vertex)
    if (!ctx.caps().provoking_vertex_last) return false;
    provoke.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    provoke.pNext = rs_next;
    rs_next = &provoke;
  }
  VkPipelineRasterizationLineStateCreateInfoEXT line_state = vk_struct<VkPipelineRasterizationLineStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT);
  if (d.line_mode != LineMode::Aliased) {
    // (kFeature2LineModes: Direct3D's alpha-antialiased lines are smooth lines - the coverage goes into the alpha -, its quadrilateral lines rectangles 1.4 or 1.0 pixels wide)
    const VenusCaps& c = ctx.caps();
    if (!c.smooth_lines || !c.rectangular_lines || c.max_line_width < 1.4f) return false;
    line_state.lineRasterizationMode = d.line_mode == LineMode::AlphaAntialiased ? VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT : VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
    rs.lineWidth = d.line_mode == LineMode::QuadrilateralWide ? 1.4f : 1.0f;
    line_state.pNext = rs_next;
    rs_next = &line_state;
  }
  rs.pNext = rs_next;
  // multisampling
  VkPipelineMultisampleStateCreateInfo ms = vk_struct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(v.key.samples ? v.key.samples : 1);
  ms.alphaToCoverageEnable = d.alpha_to_coverage && d.multisample && d.line_mode != LineMode::AlphaAntialiased ? VK_TRUE : VK_FALSE;   // (smooth lines take none)
  // (without multisampling a pixel is all or nothing by its centre: every sample of a multisampled target is put there)
  VkSampleLocationEXT centres[16];
  for (VkSampleLocationEXT& c : centres) c = {0.5f, 0.5f};
  VkPipelineSampleLocationsStateCreateInfoEXT locations = vk_struct<VkPipelineSampleLocationsStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT);
  if (v.key.flags & kVariantSampleLocations) {
    // (kFeature2SamplePositions: the positions the command buffer sets - Replay::set_sample_locations, before each draw)
    if (!(ctx.caps().sample_location_counts & ms.rasterizationSamples)) return false;
    locations.sampleLocationsEnable = VK_TRUE;
    locations.sampleLocationsInfo = vk_struct<VkSampleLocationsInfoEXT>(VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT);
    ms.pNext = &locations;
  } else if (!d.multisample && ms.rasterizationSamples > 1) {
    if (!(ctx.caps().sample_location_counts & ms.rasterizationSamples)) return false;
    locations.sampleLocationsEnable = VK_TRUE;
    locations.sampleLocationsInfo = vk_struct<VkSampleLocationsInfoEXT>(VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT);
    locations.sampleLocationsInfo.sampleLocationsPerPixel = ms.rasterizationSamples;
    locations.sampleLocationsInfo.sampleLocationGridSize = {1, 1};
    locations.sampleLocationsInfo.sampleLocationsCount = ms.rasterizationSamples;
    locations.sampleLocationsInfo.pSampleLocations = centres;
    ms.pNext = &locations;
  }
  // depth and stencil (a depth write without a test passes everything: Vulkan writes depth only when it tests)
  VkPipelineDepthStencilStateCreateInfo ds = vk_struct<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
  ds.depthTestEnable = d.depth_test || d.depth_write ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = !d.depth_test ? VK_COMPARE_OP_ALWAYS : compare_op(d.depth_compare);
  ds.stencilTestEnable = d.stencil.enable ? VK_TRUE : VK_FALSE;
  ds.front = stencil_state(d.stencil, d.stencil.enable);
  ds.back = stencil_state(d.separate_back_stencil ? d.back_stencil : d.stencil, d.stencil.enable);
  // blending (VirglDevice's rules): target 0 the pipeline's own state (blend.custom), else source over destination (alpha_blend: colour SrcAlpha / InvSrcAlpha, alpha
  // One / InvSrcAlpha), else none; with independent blending target n its own target_blend[n - 1] when that is custom, else none; without it every target as 0.
  VkPipelineColorBlendAttachmentState blends[kMaxColorTargets] = {};
  const uint32_t targets = d.color_target_count ? d.color_target_count : 1;
  auto from_state = [](const BlendState& st, VkPipelineColorBlendAttachmentState* out) {
    out->colorWriteMask = st.write_mask & 0xF;
    if (!st.enable) return;
    out->blendEnable = VK_TRUE;
    out->srcColorBlendFactor = blend_factor(st.src_color), out->dstColorBlendFactor = blend_factor(st.dst_color);
    out->srcAlphaBlendFactor = blend_factor(st.src_alpha), out->dstAlphaBlendFactor = blend_factor(st.dst_alpha);
    out->colorBlendOp = blend_op(st.color_op), out->alphaBlendOp = blend_op(st.alpha_op);
  };
  VkPipelineColorBlendAttachmentState first{};
  first.colorWriteMask = 0xF;
  if (d.blend.custom) {
    from_state(d.blend, &first);
  } else if (d.alpha_blend) {
    first.blendEnable = VK_TRUE;
    first.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, first.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    first.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, first.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    first.colorBlendOp = first.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  for (uint32_t t = 0; t < targets && t < kMaxColorTargets; ++t) {
    VkPipelineColorBlendAttachmentState& b = blends[t];
    if (t == 0 || !d.independent_blend) {
      b = first;
    } else {
      b.colorWriteMask = 0xF;
      if (d.target_blend[t - 1].custom) from_state(d.target_blend[t - 1], &b);
    }
    if (format_value_type(v.key.color_format[t]) != tgsi::ValueType::Float) b.blendEnable = VK_FALSE;   // (integer targets do not blend)
  }
  VkPipelineColorBlendStateCreateInfo cb = vk_struct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
  cb.attachmentCount = targets, cb.pAttachments = blends;
  if (d.logic_op_enable) {
    if (!ctx.caps().logic_op) return false;
    cb.logicOpEnable = VK_TRUE;
    cb.logicOp = logic_op(d.logic_op);
  }
  __builtin_memcpy(cb.blendConstants, d.blend.constant, sizeof(cb.blendConstants));
  // the state set per draw
  // (the depth bounds test - kFeature2DepthBounds - is the command buffer's state, not the pipeline's)
  // (and so are the sample locations a command buffer sets, kFeature2SamplePositions, and coarse shading's rate and combiners, kFeature2ShadingRate)
  VkDynamicState dynamic[8] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  uint32_t dynamic_count = 3;
  if (ctx.caps().depth_bounds) dynamic[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS, dynamic[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE;
  if (v.key.flags & kVariantSampleLocations) dynamic[dynamic_count++] = VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT;
  const bool rates = device_.offers_shading_rate();
  if (rates) dynamic[dynamic_count++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
  VkPipelineDynamicStateCreateInfo dyn = vk_struct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
  dyn.dynamicStateCount = dynamic_count, dyn.pDynamicStates = dynamic;
  // the attachments' formats (dynamic rendering)
  VkFormat colors[kMaxColorTargets];
  uint32_t color_count = 0;
  for (uint32_t t = 0; t < targets && t < kMaxColorTargets; ++t) colors[color_count++] = static_cast<VkFormat>(v.key.color_format[t]);
  VkPipelineRenderingCreateInfo rendering = vk_struct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
  rendering.colorAttachmentCount = color_count, rendering.pColorAttachmentFormats = colors;
  rendering.depthAttachmentFormat = static_cast<VkFormat>(v.key.depth_format);
  rendering.stencilAttachmentFormat = v.key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
  // (coarse shading: a rate of 1x1 kept by both combiners until the command buffer sets its own; a rendering may have a shading-rate image)
  VkPipelineFragmentShadingRateStateCreateInfoKHR rate = vk_struct<VkPipelineFragmentShadingRateStateCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR);
  rate.fragmentSize = {1, 1};
  rate.combinerOps[0] = rate.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
  if (rates) rate.pNext = rendering.pNext, rendering.pNext = &rate;
  VkGraphicsPipelineCreateInfo ci = vk_struct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
  ci.pNext = &rendering;
  if (rates) ci.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
  ci.stageCount = stage_count, ci.pStages = stages;
  ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia;
  ci.pTessellationState = d.hull_shader ? &ts : nullptr;
  ci.pViewportState = &vp, ci.pRasterizationState = &rs, ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &cb, ci.pDynamicState = &dyn;
  ci.layout = v.layout;
  if (vk.CreateGraphicsPipelines(device_.device(), nullptr, 1, &ci, nullptr, &v.pipeline) != VK_SUCCESS) {
    v.pipeline = nullptr;
    return false;
  }
  return true;
}

}  // namespace graphics
