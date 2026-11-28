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

// What a RenderNode does to find and use a 3D engine on its GPU. Apart from RenderNode.cpp so that the rest of the node (and what is built on it) can be
// built without the virgl and SVGA3D devices and the drivers under them.
#include "Graphics/RenderNode.hpp"
#include <new>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/IntelGen9/IntelRenderStack.hpp"
#include "Graphics/Graphics3D/Venus/KernelVulkanOs.hpp"
#include "Graphics/Graphics3D/Venus/VenusDevice.hpp"
#include "BootLog.hpp"
#ifdef TCOS_AMD_VEGAM
#include "Graphics/Graphics3D/Vega/VegaGraphicsDevice.hpp"
#endif

namespace graphics {

void RenderNode::build_accelerated(hardware::VirtioGpu* virtio, hardware::VmwareSvgaGpu* svga, hardware::IntelGpu* intel, hardware::AmdVegaM* vega, allocators::IAllocator& allocator,
                                   bool allow_acceleration, bool allow_intel_render) {
  if (!allow_acceleration) {
    reason_ = "acceleration was switched off";
  } else if (virtio) {
    if (!virtio->has_3d()) {
      reason_ = "virtio-gpu without virgl (start QEMU with virtio-gpu-gl and a gl display)";
    } else {
      // The host's Vulkan (Venus) when the device offers it and the Venus device can stand in for virgl; virgl otherwise. TCOS_GPU_BACKEND (CMake: AUTO, VENUS,
      // VIRGL) forces one for testing: VENUS takes the Venus device whenever it works, whatever virgl would do that it does not.
      venus::set_kernel_vulkan_allocator(allocator);
#if !defined(TCOS_GPU_BACKEND_VIRGL)
      if (tcos_kernel_vulkan_available()) build_venus(*virtio, allocator);
#endif
      if (!venus_device_) {
        VirglDevice* device = new (virgl_storage_) VirglDevice(*virtio);
        if (self_test(*device)) {
          gpu_device_ = device;
          active_ = device;
        } else {
          device->~VirglDevice();
          reason_ = "the virgl device failed its self-test";
        }
      }
    }
  } else if (svga) {
    if (!svga->has_3d()) {
      reason_ = "VMware SVGA adapter without a 3D engine (enable 3D acceleration in the VM settings)";
    } else {
      SvgaDevice* device = new (svga_storage_) SvgaDevice(*svga, allocator);
      if (device->initialize() && self_test(*device) && drawing_test(*device)) {
        gpu_device_ = device;
        active_ = device;
        svga_ = true;
      } else {
        device->~SvgaDevice();
        reason_ = "SVGA3D did not draw the test picture correctly";
      }
    }
#ifdef TCOS_AMD_VEGAM
  } else if (vega) {
    // The graphics engine of the Radeon RX Vega M, on the strength of its own tests: it must create every kind of object and draw the test picture, or the CPU renderer stays.
    void* memory = allocator.allocate(sizeof(VegaGraphicsDevice));
    VegaGraphicsDevice* device = memory ? new (memory) VegaGraphicsDevice(allocator, *vega) : nullptr;
    if (device && device->usable() && self_test(*device) && drawing_test(*device)) {
      gpu_device_ = device;
      active_ = device;
      vega_device_ = device;
      vega_allocator_ = &allocator;
    } else {
      reason_ = device && !device->usable() ? "the Vega M has no room for draws" : "the Vega M graphics engine did not draw the test picture";   // (before the device is gone)
      if (device) {
        device->~VegaGraphicsDevice();
        allocator.deallocate(device);
      }
    }
#else
    (void)vega;
#endif
  } else if (intel && allow_intel_render) {
    // The render engine of the Intel GPU, on the strength of its own test: the device must draw the test picture, or the CPU renderer stays.
    void* memory = allocator.allocate(sizeof(IntelRenderStack));
    IntelRenderStack* stack = memory ? new (memory) IntelRenderStack(allocator, gpu_) : nullptr;
    if (stack && stack->initialize(*intel) && self_test(stack->device()) && drawing_test(stack->device())) {
      gpu_device_ = &stack->device();
      active_ = gpu_device_;
      intel_stack_ = stack;
      intel_allocator_ = &allocator;
    } else {
      if (stack) {
        stack->~IntelRenderStack();
        allocator.deallocate(stack);
      }
      reason_ = "the Intel render engine did not start or did not draw the test picture";
    }
  } else if (intel) {
    reason_ = "the Intel render engine is off (built with TCOS_INTEL_RENDER=OFF)";
  } else {
    reason_text_.assign("the display adapter has no 3D driver: ");
    reason_text_.append(gpu_ ? gpu_->name() : "none");
    reason_ = reason_text_.c_str();
  }
  if (gpu_device_) {
    raw_device_ = gpu_device_;
    locked_ = new (locked_storage_) LockedDevice(*raw_device_);
    gpu_device_ = locked_;
    active_ = locked_;
  }
}

// The Venus device: made, tested, and (AUTO) kept only if it does all that the virgl device on the same host would - every feature, every limit - so that taking
// it costs no caller anything. The verdict is in the boot log.
void RenderNode::build_venus(hardware::VirtioGpu& virtio, allocators::IAllocator& allocator) {
  void* memory = allocator.allocate_aligned(sizeof(VenusDevice), alignof(VenusDevice));
  if (!memory) return;
  auto account = [](void* context, int64_t bytes) {   // (the virtio-gpu's video memory figure: what DXGI and the task manager show)
    hardware::VirtioGpu& gpu = *static_cast<hardware::VirtioGpu*>(context);
    if (bytes >= 0) gpu.add_resource_bytes(static_cast<uint64_t>(bytes));
    else gpu.remove_resource_bytes(static_cast<uint64_t>(-bytes));
  };
  VenusDevice* device = new (memory) VenusDevice(allocator, virtio, account, &virtio);
  bootlog::Entry e(bootlog::Level::Info, "GPU");
  bool use = false;
  if (!device->initialize()) {
    e.text("Venus: the host's Vulkan device did not open; virgl");
  } else if (!self_test(*device) || !drawing_test(*device)) {
    e.text("Venus: ").text(device->context().properties().deviceName).text(" did not draw the test picture; virgl");
  } else {
    e.text("Venus on ").text(device->context().properties().deviceName).text(" (").text(device->context().driver_name()).text(")");
#if defined(TCOS_GPU_BACKEND_VENUS)
    use = true;
    e.text(": chosen (TCOS_GPU_BACKEND=VENUS)");
#else
    // (what virgl would offer: a virgl device made for the comparison and given back)
    VirglDevice* virgl = new (virgl_storage_) VirglDevice(virtio);
    const DeviceInfo want = virgl->info(), have = device->info();
    // (the constants a draw may have: the larger of the push block and DeviceLimits::shader_constant_bytes)
    auto constant_bytes = [](const DeviceInfo& i) { return i.limits.shader_constant_bytes > i.max_push_constant_bytes ? i.limits.shader_constant_bytes : i.max_push_constant_bytes; };
    virgl->~VirglDevice();
    const uint32_t missing = want.features & ~have.features, missing2 = want.features2 & ~have.features2;
    const bool limits = have.limits.texture_units >= want.limits.texture_units && have.limits.vertex_attributes >= want.limits.vertex_attributes &&
                        have.limits.varyings >= want.limits.varyings && have.limits.color_targets >= want.limits.color_targets &&
                        have.limits.vertex_buffers >= want.limits.vertex_buffers && have.limits.control_varyings >= want.limits.control_varyings &&
                        (want.sample_counts & ~have.sample_counts) == 0 && have.max_push_constant_bytes >= want.max_push_constant_bytes &&
                        constant_bytes(have) >= constant_bytes(want) && (have.limits.depth_bias || !want.limits.depth_bias) &&
                        (have.limits.sampled_depth || !want.limits.sampled_depth) && (have.limits.stencil || !want.limits.stencil);   // (the GL translator's)
    const uint32_t gaps = device->parity_gaps();   // (host extensions the device needs to do those features as virgl does)
    use = !missing && !missing2 && limits && !gaps;
    if (use) e.text(": chosen (it does everything virgl does here)");
    else
      e.text(": virgl kept (Venus lacks features ").hex(missing).text(" / ").hex(missing2).text(limits ? "" : ", limits").text(", extensions ").hex(gaps)
          .text("; TCOS_GPU_BACKEND=VENUS forces it)");
#endif
  }
  if (!use) {
    device->~VenusDevice();
    allocator.deallocate(memory);
    return;
  }
  venus_device_ = device;
  venus_allocator_ = &allocator;
  gpu_device_ = device;
  active_ = device;
}

void RenderNode::destroy_accelerated() {
  if (locked_) locked_->~LockedDevice();
  if (venus_device_) {   // (after the locked wrapper that points to it)
    venus_device_->~VenusDevice();
    venus_allocator_->deallocate(venus_device_);
    venus_device_ = nullptr;
    raw_device_ = nullptr;
  }
  if (raw_device_ && !intel_stack_ && !vega_device_) {
    if (svga_) static_cast<SvgaDevice*>(raw_device_)->~SvgaDevice();
    else static_cast<VirglDevice*>(raw_device_)->~VirglDevice();
  }
#ifdef TCOS_AMD_VEGAM
  if (vega_device_) {   // (after the locked wrapper that points to it)
    vega_device_->~VegaGraphicsDevice();
    vega_allocator_->deallocate(vega_device_);
    vega_device_ = nullptr;
  }
#endif
  if (intel_stack_) {   // (after the locked wrapper that points into it)
    intel_stack_->~IntelRenderStack();
    intel_allocator_->deallocate(intel_stack_);
    intel_stack_ = nullptr;
  }
}

// Builds every kind of object once; a driver stack that cannot is not used.
bool RenderNode::self_test(IGraphicsDevice& device) {
  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  IImage* image = device.create_image({{16, 16}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IBuffer* buffer = device.create_buffer(64, kBufferVertex);
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = vs;
  desc.fragment_shader = fs;
  IPipeline* pipeline = vs && fs ? device.create_graphics_pipeline(desc) : nullptr;
  const bool ok = vs && fs && image && buffer && pipeline;
  if (pipeline) device.destroy_pipeline(pipeline);
  if (buffer) device.destroy_buffer(buffer);
  if (image) device.destroy_image(image);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok;
}

bool RenderNode::drawing_test(IGraphicsDevice& device) {
  constexpr uint32_t kSize = 16;
  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  IImage* image = device.create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IBuffer* buffer = device.create_buffer(sizeof(ColoredVertex) * 3, kBufferVertex);
  ICommandBuffer* commands = device.create_command_buffer();
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = vs;
  desc.fragment_shader = fs;
  desc.cull_mode = CullMode::None;
  IPipeline* pipeline = vs && fs ? device.create_graphics_pipeline(desc) : nullptr;
  bool ok = false;
  if (pipeline && image && buffer && commands) {
    // One big red triangle that covers the whole target; the matrix is the identity (its columns are the constants).
    ByteView memory = buffer->map();
    ColoredVertex* v = reinterpret_cast<ColoredVertex*>(memory.data());
    const float corners[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    for (int i = 0; i < 3; ++i) {
      v[i].position[0] = corners[i][0];
      v[i].position[1] = corners[i][1];
      v[i].position[2] = 0.5f;
      v[i].color[0] = 1; v[i].color[1] = 0; v[i].color[2] = 0; v[i].color[3] = 1;
    }
    buffer->unmap();
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    RenderPassBegin pass;
    pass.color = image;
    pass.clear_color_value = {0, 0, 1, 1};   // blue where nothing is drawn
    commands->begin();
    commands->begin_render_pass(pass);
    commands->bind_pipeline(pipeline);
    commands->bind_vertex_buffer(buffer);
    commands->push_constants(identity, sizeof identity);
    commands->draw(3);
    commands->end_render_pass();
    uint32_t pixels[kSize * kSize] = {};
    ok = commands->end() && device.submit(commands) && image->read_pixels(ArrayView<uint32_t>(pixels, kSize * kSize)) &&
         (pixels[(kSize / 2) * kSize + kSize / 2] & 0x00FFFFFF) == 0x00FF0000 && (pixels[0] & 0x00FFFFFF) == 0x00FF0000;
  }
  if (commands) device.destroy_command_buffer(commands);
  if (pipeline) device.destroy_pipeline(pipeline);
  if (buffer) device.destroy_buffer(buffer);
  if (image) device.destroy_image(image);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok;
}

}  // namespace graphics
