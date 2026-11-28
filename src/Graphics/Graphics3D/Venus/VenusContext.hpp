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

// The host's Vulkan device as the kernel holds it (KernelVulkan.h): one instance, the first physical device that has a graphics and compute queue, a device with
// the features and extensions the Venus graphics device uses when the host has them, and one queue. Made once at boot (open()); every call after that is the
// kernel Vulkan table's (vk()), so callers use Vulkan as a program does.
#pragma once
#include <stdint.h>
#include "Graphics/Graphics3D/Venus/KernelVulkan.h"

namespace graphics::venus {

// A Vulkan structure with its type set and everything else zero.
template <class T>
T vk_struct(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

// What the device was made with (each true only when the host has it and it was enabled).
struct VenusCaps {
  bool transform_feedback = false;       // VK_EXT_transform_feedback (stream output)
  bool geometry_streams = false;         //  ... with more than one vertex stream
  bool conditional_rendering = false;    // VK_EXT_conditional_rendering (predication)
  bool custom_border_color = false;      // VK_EXT_custom_border_color
  bool conservative_rasterization = false;   // VK_EXT_conservative_rasterization
  bool depth_clip_enable = false;        // VK_EXT_depth_clip_enable
  bool index_type_uint8 = false;         // VK_EXT_index_type_uint8
  bool line_rasterization = false;       // VK_EXT_line_rasterization
  bool vertex_attribute_divisor = false, vertex_attribute_zero_divisor = false;   // VK_EXT_vertex_attribute_divisor
  bool provoking_vertex_last = false;    // VK_EXT_provoking_vertex (GL's flat varyings)
  bool null_descriptor = false, robust_buffer_access2 = false, robust_image_access2 = false;   // VK_EXT_robustness2 (unbound resources read 0)
  bool sample_locations = false;         // VK_EXT_sample_locations (a pipeline without multisampling on a multisampled target: every sample at the centre)
  uint32_t sample_location_counts = 0;
  bool depth_bias_float = false;         // VK_EXT_depth_bias_control with the float representation (normalised depth formats' bias on D32F)
  bool depth_bounds = false;             // depthBounds (Direct3D 12's depth bounds test)
  bool sampler_minmax = false;           // samplerFilterMinmax (Direct3D's MINIMUM_* / MAXIMUM_* filters)
  bool smooth_lines = false, rectangular_lines = false;   // VK_EXT_line_rasterization's smooth and rectangular lines (Direct3D's line modes)
  bool pixel_interlock = false;          // VK_EXT_fragment_shader_interlock's fragmentShaderPixelInterlock (Direct3D's rasterizer ordered views)
  float max_line_width = 1;              // (with wideLines; Direct3D's wide quadrilateral lines are 1.4 pixels)
  bool shader_float64 = false;
  bool geometry_shader = false, tessellation_shader = false, dual_src_blend = false, logic_op = false, sampler_anisotropy = false;
  bool multi_viewport = false, shader_output_layer = false, shader_output_viewport_index = false;
  bool independent_blend = false, depth_clamp = false, fill_mode_non_solid = false, wide_lines = false, large_points = false;
  bool image_cube_array = false, shader_storage_image_write_without_format = false, shader_storage_image_read_without_format = false;
  bool fragment_stores_and_atomics = false, vertex_pipeline_stores_and_atomics = false, sample_rate_shading = false, occlusion_query_precise = false;
  bool pipeline_statistics_query = false, shader_clip_distance = false, shader_cull_distance = false, texture_compression_bc = false;
  bool shader_image_gather_extended = false, draw_indirect_first_instance = false, multi_draw_indirect = false;
  float max_anisotropy = 1;
  uint32_t timestamp_valid_bits = 0;
  float timestamp_period = 1;
  // Direct3D 12's later rasterizer features (gap pass 5):
  // programmable sample positions (VK_EXT_sample_locations' dynamic locations): the counts with a 2 x 2 grid of pixels (bit n: n samples), whether pipelines may
  // differ in their locations within a rendering
  uint32_t sample_location_quad_counts = 0;
  bool variable_sample_locations = false;
  // coarse pixel shading (VK_KHR_fragment_shading_rate): the pipeline's, the primitive's and the attachment's rates with every combiner, the attachment's texel
  // size (a square), the largest coarse pixel (each axis 1, 2 or 4) and the most samples a coarse pixel takes
  bool shading_rate = false;
  uint32_t shading_rate_texel = 0, max_fragment_width = 1, max_fragment_height = 1, shading_rate_samples = 0;
  bool fragment_barycentric = false;     // VK_KHR_fragment_shader_barycentric (SV_Barycentrics, GetAttributeAtVertex)
  // sparse residency (Direct3D's tiled resources tier 2 and up): 2D and 3D images, the shaders' residency codes and minimum level of detail, the standard block
  // shapes, unbound texels reading zero, and a queue that binds sparse memory
  bool sparse_residency_2d = false, sparse_residency_3d = false, shader_resource_residency = false, shader_resource_min_lod = false;
  bool residency_standard_2d = false, residency_standard_3d = false, residency_strict = false, sparse_queue = false;
};

class VenusContext {
 public:
  VenusContext() = default;
  ~VenusContext();
  VenusContext(const VenusContext&) = delete;
  VenusContext& operator=(const VenusContext&) = delete;

  // Makes the instance, picks the physical device and makes the device and its queue. False (and nothing made) when the system has no Venus or the host's
  // device lacks what the graphics device needs at the least (Vulkan 1.3 with dynamic rendering, a queue for graphics and compute).
  [[nodiscard]] bool open();
  void close();
  bool opened() const { return device_ != nullptr; }

  const TcosKernelVulkan& vk() const { return *vk_; }
  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return family_; }
  const VkPhysicalDeviceProperties& properties() const { return properties_; }
  const VkPhysicalDeviceMemoryProperties& memory() const { return memory_; }
  const VenusCaps& caps() const { return caps_; }
  // The host driver's name (VkPhysicalDeviceDriverProperties::driverName), "" if it did not say.
  const char* driver_name() const { return driver_name_; }

  // A memory type among `bits` that has all of `need`, preferring one that also has `prefer`; -1 if none.
  int32_t memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer = 0) const;
  // vkGetPhysicalDeviceFormatProperties of the device, from what open() asked once for every core format and the extension formats the device uses (any
  // other is asked of the host).
  VkFormatProperties format_properties(VkFormat format) const;

 private:
  const TcosKernelVulkan* vk_ = nullptr;
  VkInstance instance_ = nullptr;
  VkPhysicalDevice physical_ = nullptr;
  VkDevice device_ = nullptr;
  VkQueue queue_ = nullptr;
  uint32_t family_ = 0;
  VkPhysicalDeviceProperties properties_{};
  VkPhysicalDeviceMemoryProperties memory_{};
  VenusCaps caps_;
  char driver_name_[VK_MAX_DRIVER_NAME_SIZE] = {};
  static constexpr uint32_t kCachedCoreFormats = VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1;   // (Vulkan 1.0's formats are numbered 0 to this one)
  static constexpr VkFormat kExtensionFormats[] = {VK_FORMAT_A4R4G4B4_UNORM_PACK16, VK_FORMAT_A4B4G4R4_UNORM_PACK16};
  static constexpr uint32_t kCachedExtensionFormats = sizeof(kExtensionFormats) / sizeof(kExtensionFormats[0]);
  VkFormatProperties core_formats_[kCachedCoreFormats] = {};
  VkFormatProperties extension_formats_[kCachedExtensionFormats] = {};
};

}  // namespace graphics::venus
