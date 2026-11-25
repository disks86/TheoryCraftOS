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

#include "Graphics/Graphics3D/Venus/VenusContext.hpp"
#include "Utility/StringView.hpp"

namespace graphics::venus {
namespace {

constexpr uint32_t kMaxPhysicalDevices = 8;
constexpr uint32_t kMaxQueueFamilies = 16;
constexpr uint32_t kMaxExtensions = 512;

bool same(const char* a, const char* b) { return StringView(a) == StringView(b); }

// The device extensions the graphics device uses when the host has them (each optional: what it brings is reported as a VenusCaps flag).
const char* const kWanted[] = {
    VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,      VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,   VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME,
    VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,      VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,            VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME,
};
constexpr uint32_t kWantedCount = sizeof(kWanted) / sizeof(kWanted[0]);

}  // namespace

VenusContext::~VenusContext() { close(); }

int32_t VenusContext::memory_type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer) const {
  int32_t found = -1;
  for (uint32_t i = 0; i < memory_.memoryTypeCount && i < 32; ++i) {
    if (!(bits & (1u << i))) continue;
    const VkMemoryPropertyFlags f = memory_.memoryTypes[i].propertyFlags;
    if ((f & need) != need) continue;
    if ((f & prefer) == prefer) return static_cast<int32_t>(i);
    if (found < 0) found = static_cast<int32_t>(i);
  }
  return found;
}

bool VenusContext::open() {
  if (device_) return true;
  vk_ = tcos_kernel_vulkan();
  const VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "TheoryCraftOS kernel", 1, "TheoryCraftOS", 1, VK_API_VERSION_1_3};
  const VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
  if (vk_->CreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
    instance_ = nullptr;
    return false;
  }
  VkPhysicalDevice list[kMaxPhysicalDevices] = {};
  uint32_t count = kMaxPhysicalDevices;
  const VkResult listed = vk_->EnumeratePhysicalDevices(instance_, &count, list);
  if (listed != VK_SUCCESS && listed != VK_INCOMPLETE) {
    close();
    return false;
  }
  // (the first device of Vulkan 1.3 with dynamic rendering and one queue family for graphics and compute; the host lists its discrete GPU first)
  for (uint32_t d = 0; d < count && !physical_; ++d) {
    VkPhysicalDeviceProperties p{};
    vk_->GetPhysicalDeviceProperties(list[d], &p);
    if (p.apiVersion < VK_API_VERSION_1_3) continue;
    VkQueueFamilyProperties families[kMaxQueueFamilies] = {};
    uint32_t n = kMaxQueueFamilies;
    vk_->GetPhysicalDeviceQueueFamilyProperties(list[d], &n, families);
    for (uint32_t f = 0; f < n; ++f)
      if ((families[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
        physical_ = list[d];
        family_ = f;
        properties_ = p;
        caps_.timestamp_valid_bits = families[f].timestampValidBits;
        break;
      }
  }
  if (!physical_) {
    close();
    return false;
  }
  vk_->GetPhysicalDeviceMemoryProperties(physical_, &memory_);
  caps_.timestamp_period = properties_.limits.timestampPeriod;
  caps_.max_anisotropy = properties_.limits.maxSamplerAnisotropy;

  // The extensions the host has, of those wanted.
  VkExtensionProperties* have = static_cast<VkExtensionProperties*>(tcos_kvk_alloc(sizeof(VkExtensionProperties) * kMaxExtensions));
  if (!have) {
    close();
    return false;
  }
  uint32_t have_count = kMaxExtensions;
  if (vk_->EnumerateDeviceExtensionProperties(physical_, nullptr, &have_count, have) < 0) have_count = 0;
  const char* enable[kWantedCount] = {};
  uint32_t enable_count = 0;
  auto has = [&](const char* name) {
    for (uint32_t i = 0; i < have_count; ++i)
      if (same(have[i].extensionName, name)) return true;
    return false;
  };
  for (const char* name : kWanted)
    if (has(name)) enable[enable_count++] = name;
  tcos_kvk_free(have);
  auto enabled = [&](const char* name) {
    for (uint32_t i = 0; i < enable_count; ++i)
      if (same(enable[i], name)) return true;
    return false;
  };

  // Features: everything of Vulkan 1.0 - 1.3 the host has (they cost nothing unused; robust buffer access is what Direct3D's out-of-range reads want), and the
  // features of the extensions enabled.
  auto xfb = vk_struct<VkPhysicalDeviceTransformFeedbackFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
  auto cond = vk_struct<VkPhysicalDeviceConditionalRenderingFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT);
  auto border = vk_struct<VkPhysicalDeviceCustomBorderColorFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
  auto clip = vk_struct<VkPhysicalDeviceDepthClipEnableFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
  auto u8 = vk_struct<VkPhysicalDeviceIndexTypeUint8FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT);
  auto lines = vk_struct<VkPhysicalDeviceLineRasterizationFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT);
  auto divisor = vk_struct<VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
  auto provoking = vk_struct<VkPhysicalDeviceProvokingVertexFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
  auto robust2 = vk_struct<VkPhysicalDeviceRobustness2FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
  auto bias = vk_struct<VkPhysicalDeviceDepthBiasControlFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT);
  auto v13 = vk_struct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
  auto v12 = vk_struct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  auto v11 = vk_struct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
  auto features = vk_struct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
  void* chain = nullptr;
  auto link = [&](auto& s, bool when) {
    if (!when) return;
    s.pNext = chain;
    chain = &s;
  };
  link(xfb, enabled(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME));
  link(cond, enabled(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME));
  link(border, enabled(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME));
  link(clip, enabled(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME));
  link(u8, enabled(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME));
  link(lines, enabled(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME));
  link(divisor, enabled(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME));
  link(provoking, enabled(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME));
  link(robust2, enabled(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME));
  link(bias, enabled(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME));
  link(v13, true);
  link(v12, true);
  link(v11, true);
  features.pNext = chain;
  vk_->GetPhysicalDeviceFeatures2(physical_, &features);
  if (!v13.dynamicRendering || !v13.synchronization2) {
    close();
    return false;
  }
  // (what is not used and has a cost or a rule of its own is left off)
  v12.bufferDeviceAddressCaptureReplay = VK_FALSE, v12.bufferDeviceAddressMultiDevice = VK_FALSE;
  cond.inheritedConditionalRendering = VK_FALSE;
  provoking.transformFeedbackPreservesProvokingVertex = VK_FALSE;
  bias.leastRepresentableValueForceUnormRepresentation = VK_FALSE;
  lines.stippledRectangularLines = lines.stippledBresenhamLines = lines.stippledSmoothLines = VK_FALSE;

  const VkPhysicalDeviceFeatures& f = features.features;
  caps_.geometry_shader = f.geometryShader, caps_.tessellation_shader = f.tessellationShader, caps_.dual_src_blend = f.dualSrcBlend, caps_.logic_op = f.logicOp;
  caps_.sampler_anisotropy = f.samplerAnisotropy, caps_.multi_viewport = f.multiViewport, caps_.independent_blend = f.independentBlend, caps_.depth_clamp = f.depthClamp;
  caps_.fill_mode_non_solid = f.fillModeNonSolid, caps_.wide_lines = f.wideLines, caps_.large_points = f.largePoints, caps_.image_cube_array = f.imageCubeArray;
  caps_.shader_storage_image_write_without_format = f.shaderStorageImageWriteWithoutFormat;
  caps_.shader_storage_image_read_without_format = f.shaderStorageImageReadWithoutFormat;
  caps_.fragment_stores_and_atomics = f.fragmentStoresAndAtomics, caps_.vertex_pipeline_stores_and_atomics = f.vertexPipelineStoresAndAtomics;
  caps_.sample_rate_shading = f.sampleRateShading, caps_.occlusion_query_precise = f.occlusionQueryPrecise, caps_.pipeline_statistics_query = f.pipelineStatisticsQuery;
  caps_.shader_clip_distance = f.shaderClipDistance, caps_.shader_cull_distance = f.shaderCullDistance, caps_.texture_compression_bc = f.textureCompressionBC;
  caps_.shader_image_gather_extended = f.shaderImageGatherExtended, caps_.draw_indirect_first_instance = f.drawIndirectFirstInstance;
  caps_.multi_draw_indirect = f.multiDrawIndirect, caps_.shader_float64 = f.shaderFloat64;
  caps_.shader_output_layer = v12.shaderOutputLayer, caps_.shader_output_viewport_index = v12.shaderOutputViewportIndex;
  caps_.transform_feedback = enabled(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) && xfb.transformFeedback;
  caps_.geometry_streams = caps_.transform_feedback && xfb.geometryStreams;
  caps_.conditional_rendering = enabled(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) && cond.conditionalRendering;
  caps_.custom_border_color = enabled(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) && border.customBorderColors;
  caps_.conservative_rasterization = enabled(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
  caps_.depth_clip_enable = enabled(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) && clip.depthClipEnable;
  caps_.index_type_uint8 = enabled(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME) && u8.indexTypeUint8;
  caps_.line_rasterization = enabled(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME);
  caps_.vertex_attribute_divisor = enabled(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) && divisor.vertexAttributeInstanceRateDivisor;
  caps_.vertex_attribute_zero_divisor = caps_.vertex_attribute_divisor && divisor.vertexAttributeInstanceRateZeroDivisor;
  caps_.provoking_vertex_last = enabled(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) && provoking.provokingVertexLast;
  caps_.null_descriptor = enabled(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) && robust2.nullDescriptor;
  caps_.robust_buffer_access2 = enabled(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) && robust2.robustBufferAccess2;
  caps_.robust_image_access2 = enabled(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) && robust2.robustImageAccess2;
  caps_.depth_bias_float = enabled(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME) && bias.depthBiasControl && bias.floatRepresentation && bias.depthBiasExact;

  // The driver's name (for the boot log and GL_RENDERER).
  auto driver = vk_struct<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
  auto p2 = vk_struct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
  p2.pNext = &driver;
  vk_->GetPhysicalDeviceProperties2(physical_, &p2);
  for (uint32_t i = 0; i + 1 < sizeof(driver_name_) && driver.driverName[i]; ++i) driver_name_[i] = driver.driverName[i];

  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family_, 1, &priority};
  const VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features, 0, 1, &qci, 0, nullptr, enable_count, enable, nullptr};
  if (vk_->CreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
    device_ = nullptr;
    close();
    return false;
  }
  const VkDeviceQueueInfo2 qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, nullptr, 0, family_, 0};
  vk_->GetDeviceQueue2(device_, &qi, &queue_);
  if (!queue_) {
    close();
    return false;
  }
  return true;
}

void VenusContext::close() {
  if (device_) {
    vk_->DeviceWaitIdle(device_);
    vk_->DestroyDevice(device_, nullptr);
  }
  if (instance_) vk_->DestroyInstance(instance_, nullptr);
  device_ = nullptr, queue_ = nullptr, physical_ = nullptr, instance_ = nullptr;
}

}  // namespace graphics::venus
