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
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,            VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME,       VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
    VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
    VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,   VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
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
        caps_.sparse_queue = (families[f].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
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
  auto interlock = vk_struct<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
  auto rate = vk_struct<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR);
  auto bary = vk_struct<VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR);
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
  link(interlock, enabled(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME));
  link(rate, enabled(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME));
  link(bary, enabled(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME));
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
  interlock.fragmentShaderShadingRateInterlock = VK_FALSE;
  // (sparse memory only where a queue binds it; the aliased and the buffer kinds are not used)
  if (!caps_.sparse_queue) {
    features.features.sparseBinding = VK_FALSE;
    features.features.sparseResidencyBuffer = features.features.sparseResidencyImage2D = features.features.sparseResidencyImage3D = VK_FALSE;
    features.features.sparseResidency2Samples = features.features.sparseResidency4Samples = features.features.sparseResidency8Samples = VK_FALSE;
    features.features.sparseResidency16Samples = features.features.sparseResidencyAliased = VK_FALSE;
  }
  features.features.sparseResidencyAliased = VK_FALSE;

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
  caps_.depth_bounds = f.depthBounds;
  caps_.sampler_minmax = v12.samplerFilterMinmax;
  caps_.max_line_width = f.wideLines ? properties_.limits.lineWidthRange[1] : 1.0f;
  caps_.smooth_lines = caps_.line_rasterization && lines.smoothLines;
  caps_.rectangular_lines = caps_.line_rasterization && lines.rectangularLines;
  caps_.pixel_interlock = enabled(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) && interlock.fragmentShaderPixelInterlock;
  caps_.fragment_barycentric = enabled(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) && bary.fragmentShaderBarycentric;
  caps_.sparse_residency_2d = caps_.sparse_queue && f.sparseBinding && f.sparseResidencyImage2D;
  caps_.sparse_residency_3d = caps_.sparse_queue && f.sparseBinding && f.sparseResidencyImage3D;
  caps_.shader_resource_residency = f.shaderResourceResidency, caps_.shader_resource_min_lod = f.shaderResourceMinLod;

  // The driver's name (for the boot log and GL_RENDERER).
  auto driver = vk_struct<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
  auto locations = vk_struct<VkPhysicalDeviceSampleLocationsPropertiesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT);
  auto rate_props = vk_struct<VkPhysicalDeviceFragmentShadingRatePropertiesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR);
  auto p2 = vk_struct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
  p2.pNext = &driver;
  if (enabled(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME)) locations.pNext = p2.pNext, p2.pNext = &locations;
  if (enabled(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) rate_props.pNext = p2.pNext, p2.pNext = &rate_props;
  vk_->GetPhysicalDeviceProperties2(physical_, &p2);
  for (uint32_t i = 0; i + 1 < sizeof(driver_name_) && driver.driverName[i]; ++i) driver_name_[i] = driver.driverName[i];
  caps_.sample_locations = enabled(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
  caps_.sample_location_counts = caps_.sample_locations ? locations.sampleLocationSampleCounts : 0;
  if (caps_.sample_locations) {
    // (Direct3D's positions are sixteenths of a pixel, -8 .. 7 from the centre: the host must place a sample anywhere in 0 .. 15/16 at that precision; a quad's
    // patterns need a grid of 2 x 2 for the count)
    caps_.variable_sample_locations = locations.variableSampleLocations;
    const bool precise = locations.sampleLocationSubPixelBits >= 4 && locations.sampleLocationCoordinateRange[0] <= 0.0f && locations.sampleLocationCoordinateRange[1] >= 0.9375f;
    for (uint32_t n = 1; precise && n <= 16; n *= 2) {
      if (!(caps_.sample_location_counts & n)) continue;
      auto mp = vk_struct<VkMultisamplePropertiesEXT>(VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT);
      vk_->GetPhysicalDeviceMultisamplePropertiesEXT(physical_, static_cast<VkSampleCountFlagBits>(n), &mp);
      if (mp.maxSampleLocationGridSize.width >= 2 && mp.maxSampleLocationGridSize.height >= 2) caps_.sample_location_quad_counts |= 1u << n;
    }
  }
  if (enabled(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) && rate.pipelineFragmentShadingRate && rate.primitiveFragmentShadingRate && rate.attachmentFragmentShadingRate &&
      rate_props.fragmentShadingRateNonTrivialCombinerOps && rate_props.fragmentShadingRateStrictMultiplyCombiner && rate_props.primitiveFragmentShadingRateWithMultipleViewports &&
      rate_props.minFragmentShadingRateAttachmentTexelSize.width == rate_props.minFragmentShadingRateAttachmentTexelSize.height &&
      rate_props.minFragmentShadingRateAttachmentTexelSize.width <= 16 && 16 % rate_props.minFragmentShadingRateAttachmentTexelSize.width == 0) {
    caps_.shading_rate = true;
    caps_.shading_rate_texel = rate_props.minFragmentShadingRateAttachmentTexelSize.width;
    caps_.max_fragment_width = rate_props.maxFragmentSize.width < 4 ? rate_props.maxFragmentSize.width : 4;
    caps_.max_fragment_height = rate_props.maxFragmentSize.height < 4 ? rate_props.maxFragmentSize.height : 4;
    caps_.shading_rate_samples = rate_props.maxFragmentShadingRateCoverageSamples;
  } else {
    rate.pipelineFragmentShadingRate = rate.primitiveFragmentShadingRate = rate.attachmentFragmentShadingRate = VK_FALSE;
  }
  // (the sparse image properties: the standard block shapes, and what unbound texels read)
  caps_.residency_standard_2d = properties_.sparseProperties.residencyStandard2DBlockShape;
  caps_.residency_standard_3d = properties_.sparseProperties.residencyStandard3DBlockShape;
  caps_.residency_strict = properties_.sparseProperties.residencyNonResidentStrict;

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
