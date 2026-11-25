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

// The Venus device's objects (VenusDevice.hpp): buffers, images, shader modules, pipelines and command buffers. Destructors are not virtual: the device destroys
// each through its own type (destroy_buffer casts to VenusBuffer, and so on).
#pragma once
#include "Graphics/Graphics3D/CommandRecorder.hpp"
#include "Graphics/Graphics3D/Venus/TgsiSpirv.hpp"
#include "Graphics/Graphics3D/Venus/TgsiText.hpp"
#include "Graphics/Graphics3D/Venus/VenusDevice.hpp"
#include "ObjectContainer/Hive.hpp"
#include "Utility/ByteBuffer.hpp"

namespace graphics {

class VenusBuffer final : public IBuffer {
 public:
  VenusBuffer(VenusDevice& device, uint64_t size) : device_(device), size_(size) {}
  uint64_t size() const override { return size_; }
  ByteView map() override;
  void unmap() override {}   // (host-coherent memory: the GPU sees what was written)
  bool read_back() override { return true; }   // (submit waits for the GPU and makes its writes visible to the host)
  VkBuffer buffer() const { return memory_.buffer; }

 private:
  friend class VenusDevice;
  VenusDevice& device_;
  uint64_t size_;
  VenusMemory memory_;
};

class VenusImage final : public IImage {
 public:
  VenusImage(VenusDevice& device, const ImageDesc& desc);
  ~VenusImage();
  bool create();

  Extent2D extent() const override { return desc_.extent; }
  Format format() const override { return desc_.format; }
  bool read_pixels(ArrayView<uint32_t> out) override;
  bool write_pixels(ArrayView<const uint32_t> pixels) override;
  bool write_rect(const Rect2D& area, ArrayView<const uint32_t> pixels, uint32_t pitch) override;
  bool read_pixels_float(ArrayView<Vec4> out) override;
  bool write_pixels_float(ArrayView<const Vec4> pixels) override;
  uint32_t layer_count() const override { return layers_; }
  bool write_layer_pixels(uint32_t layer, ArrayView<const uint32_t> pixels) override;
  bool read_layer_pixels(uint32_t layer, ArrayView<uint32_t> out) override;
  bool write_layer_pixels_float(uint32_t layer, ArrayView<const Vec4> pixels) override;
  bool read_layer_pixels_float(uint32_t layer, ArrayView<Vec4> out) override;
  uint32_t mip_levels() const override { return desc_.mip_levels; }
  bool write_mip_pixels(uint32_t level, ArrayView<const uint32_t> pixels) override;
  bool read_mip_pixels(uint32_t level, ArrayView<uint32_t> out) override;
  bool write_compressed(uint32_t level, uint32_t layer, ConstByteView blocks) override;
  bool write_subresource(uint32_t level, uint32_t layer, ArrayView<const uint32_t> pixels) override;
  bool read_subresource(uint32_t level, uint32_t layer, ArrayView<uint32_t> out) override;
  bool write_subresource_float(uint32_t level, uint32_t layer, ArrayView<const Vec4> pixels) override;
  bool read_subresource_float(uint32_t level, uint32_t layer, ArrayView<Vec4> out) override;
  bool generate_mipmaps() override;

  const ImageDesc& desc() const { return desc_; }
  VkImage image() const { return image_; }
  VkFormat vk_format() const { return format_; }
  VkImageAspectFlags aspect() const { return aspect_; }
  bool swaps_rb() const { return swap_rb_; }
  uint32_t level_layers(uint32_t level) const;
  VkImageView sampled_view(const TextureView& view, bool stencil);
  VkImageView whole_view(bool stencil);
  VkImageView storage_view(uint32_t level, uint32_t first_layer, uint32_t layer_count, VkFormat format);
  VkImageView target_view(uint32_t level, uint32_t first_layer, uint32_t layer_count);
  // Copies one level of one layer between the image and the device's staging buffer (rows of `row_texels`).
  bool copy(bool to_image, uint32_t level, uint32_t layer, uint32_t row_texels);

 private:
  uint32_t stage(ArrayView<const uint32_t> pixels);
  bool unstage(ArrayView<uint32_t> out);
  bool stage_wide(ArrayView<const Vec4> pixels);
  bool unstage_wide(ArrayView<Vec4> out);
  bool transferable(uint32_t level, uint32_t layer, uint64_t count) const;

  struct View {
    TextureView v;
    bool stencil = false, storage = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageView view = nullptr;
  };
  struct Target {
    uint32_t level = 0, first = 0, count = 0;
    VkImageView view = nullptr;
  };
  VenusDevice& device_;
  ImageDesc desc_;
  VkImage image_ = nullptr;
  VkDeviceMemory memory_ = nullptr;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t layers_ = 1;
  bool swap_rb_ = false;
  uint64_t accounted_ = 0;
  List<View> views_;
  List<Target> targets_;
};

// A shader module: its TGSI text (checked by the reader at creation) and what its description says; compiled to SPIR-V per pipeline variant.
class VenusShaderModule final : public IShaderModule {
 public:
  VenusShaderModule(allocators::IAllocator& allocator, const ShaderModuleDesc& desc) : desc_(desc), text_(allocator), program_(allocator) {}
  ShaderStage stage() const override { return desc_.stage; }
  bool create();
  const ShaderModuleDesc& desc() const { return desc_; }
  const tgsi::Program& program() const { return program_; }
  StringView text() const { return StringView(text_.view()); }

 private:
  ShaderModuleDesc desc_;
  ByteBuffer text_;
  tgsi::Program program_;
};

// What a pipeline variant depends on besides the pipeline itself: the value types of the bound textures and storage images (a shader's sampled types must match
// the formats), the storage images' formats, the attachments' formats and samples (dynamic rendering bakes them in), the primitive topology and the patch's
// control points. (All of it 32-bit words or byte arrays of multiples of four: compared as bytes.)
struct VenusVariantKey {
  uint8_t unit_type[tgsi::kMaxUnits] = {};                                  // texture unit u's value type (tgsi::ValueType)
  uint8_t texel_type[tgsi::kMaxStages][tgsi::kMaxTexelViews] = {};          // each stage's typed buffer view SVIEW[16 + k]
  uint8_t image_format[tgsi::kMaxStages][tgsi::kMaxImages] = {};            // each stage's IMAGE[k] (tgsi::ImageFormat)
  uint32_t color_format[kMaxColorTargets] = {};
  uint32_t depth_format = 0;
  uint32_t samples = 1;
  uint32_t topology = 0;
  uint32_t patch_points = 0;
  bool operator==(const VenusVariantKey& o) const { return __builtin_memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(VenusVariantKey) == tgsi::kMaxUnits + tgsi::kMaxStages * (tgsi::kMaxTexelViews + tgsi::kMaxImages) + 4 * (kMaxColorTargets + 4),
              "no padding: keys are compared as bytes");

class VenusPipeline final : public IPipeline {
 public:
  VenusPipeline(VenusDevice& device, const GraphicsPipelineDesc& desc) : device_(device), desc_(desc), variants_(device.allocator()), sets_(device.allocator()) {}
  VenusPipeline(VenusDevice& device, const VenusShaderModule* compute) : device_(device), compute_(compute), variants_(device.allocator()), sets_(device.allocator()) {}
  ~VenusPipeline();
  bool compute() const { return compute_ != nullptr; }
  const GraphicsPipelineDesc& desc() const { return desc_; }
  const VenusShaderModule* compute_module() const { return compute_; }
  // The vertex buffer slot binding b of the pipeline's vertex input reads, and how many bindings there are.
  uint32_t binding_slot(uint32_t b) const { return binding_slot_[b]; }
  uint32_t binding_count() const { return binding_count_; }
  // The stages' modules, in the order of the pipeline's stage slots (CompileOptions::stage_slot).
  uint32_t stage_modules(const VenusShaderModule** out) const;

  // A variant made for one binding shape.
  struct Variant {
    VenusVariantKey key;
    VkPipeline pipeline = nullptr;
    VkPipelineLayout layout = nullptr;
    VkDescriptorSetLayout set_layout = nullptr;
    // the resources the shaders use (bit masks over tgsi's numbering; storage per stage)
    uint32_t unit_mask = 0, sampler_mask = 0;
    uint32_t texel_mask[tgsi::kMaxStages] = {};
    uint64_t storage_mask[tgsi::kMaxStages] = {}, image_mask[tgsi::kMaxStages] = {}, image_buffer_mask[tgsi::kMaxStages] = {};
    const VenusShaderModule* stage_module[tgsi::kMaxStages] = {};   // (whose TgsiBindings number the stage's storage)
    uint32_t stage_count = 0;
    bool constants = false;
    uint32_t sets_used = 0;           // this submit's
  };
  // The variant for `key`, made on first use. nullptr when the shaders do not compile for it or the host refuses the pipeline.
  Variant* variant(const VenusVariantKey& key);
  // A descriptor set of the variant's layout this submit has not used yet (made once, reused by later submits). nullptr when the pool is out of sets.
  VkDescriptorSet next_set(Variant& v);
  void reset_sets() {
    for (Variant& v : variants_) v.sets_used = 0;
  }

 private:
  bool make(Variant& v);
  bool make_graphics(Variant& v, const VkPipelineShaderStageCreateInfo* stages, uint32_t stage_count);
  VenusDevice& device_;
  GraphicsPipelineDesc desc_{};
  const VenusShaderModule* compute_ = nullptr;
  uint8_t binding_slot_[kMaxVertexBuffers] = {};
  uint32_t binding_count_ = 0;
  Hive<Variant> variants_;   // (pointers to variants stay valid while others are made)
  struct Set {
    const Variant* variant = nullptr;
    VkDescriptorSet set = nullptr;
    VkDescriptorPool pool = nullptr;
  };
  List<Set> sets_;
};

// The command buffer: CommandRecorder's list, replayed by VenusDevice::submit.
class VenusCommands final : public CommandRecorder {
 public:
  explicit VenusCommands(allocators::IAllocator& allocator) : CommandRecorder(allocator) {}
};

}  // namespace graphics
