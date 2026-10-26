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

// A graphics device that many threads share: every call into the real device is made under one mutex. The GPU drivers behind the devices keep state (object
// ids, the command stream being built, fences) and are not thread-safe, while the desktop's renderer, the 3D test window and the browser's canvases each run on
// their own threads. Recording into a command buffer is not locked - a command buffer belongs to one user - only what reaches the device is: creating and
// destroying objects, submit() and present().
#pragma once
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Kernel/Sync.hpp"

namespace graphics {

class LockedDevice final : public IGraphicsDevice {
 public:
  explicit LockedDevice(IGraphicsDevice& inner) : inner_(inner) {}

  DeviceInfo info() const override { return inner_.info(); }
  IBuffer* create_buffer(uint64_t size, uint32_t usage) override { kernel::ScopedMutex g(lock_); return inner_.create_buffer(size, usage); }
  void destroy_buffer(IBuffer* buffer) override { kernel::ScopedMutex g(lock_); inner_.destroy_buffer(buffer); }
  IImage* create_image(const ImageDesc& desc) override { kernel::ScopedMutex g(lock_); return inner_.create_image(desc); }
  void destroy_image(IImage* image) override { kernel::ScopedMutex g(lock_); inner_.destroy_image(image); }
  IShaderModule* create_shader_module(const ShaderModuleDesc& desc) override { kernel::ScopedMutex g(lock_); return inner_.create_shader_module(desc); }
  void destroy_shader_module(IShaderModule* module) override { kernel::ScopedMutex g(lock_); inner_.destroy_shader_module(module); }
  IPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) override { kernel::ScopedMutex g(lock_); return inner_.create_graphics_pipeline(desc); }
  void destroy_pipeline(IPipeline* pipeline) override { kernel::ScopedMutex g(lock_); inner_.destroy_pipeline(pipeline); }
  ICommandBuffer* create_command_buffer() override { kernel::ScopedMutex g(lock_); return inner_.create_command_buffer(); }
  void destroy_command_buffer(ICommandBuffer* commands) override { kernel::ScopedMutex g(lock_); inner_.destroy_command_buffer(commands); }
  bool submit(ICommandBuffer* commands) override { kernel::ScopedMutex g(lock_); return inner_.submit(commands); }
  bool present(IImage* image) override { kernel::ScopedMutex g(lock_); return inner_.present(image); }

 private:
  IGraphicsDevice& inner_;
  kernel::Mutex lock_;
};

}  // namespace graphics
