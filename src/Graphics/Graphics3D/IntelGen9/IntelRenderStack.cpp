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

#include "Graphics/Graphics3D/IntelGen9/IntelRenderStack.hpp"

namespace graphics {

bool IntelRenderStack::initialize(hardware::IntelGpu& gpu) {
  using namespace hardware::intel_gt;
  constexpr uint64_t kGuardPages = 16;   // (as SystemVideoCodecs leaves at the top of the table)
  hardware::IntelRenderHardware hw;
  if (device_ || !gpu.render_hardware(&hw)) return false;
  VcsDriver::Config config;
  config.registers = hw.registers;
  config.ggtt_table = hw.ggtt_table;
  config.generation = Generation::Gen9;
  config.device_id = hw.device_id;
  config.revision = hw.revision;
  config.engine = VcsDriver::Engine::Render;
  // Pages nothing maps now; the driver maps them in initialize(), so the video engine driver, which looks later, finds others.
  if (!find_unused_ggtt_range(hw.ggtt_table, VcsDriver::kRenderGgttPagesNeeded, kGuardPages, &config.ggtt_first_page)) return false;
  if (!driver_.initialize(config)) return false;
  device_ = UniquePointer<Gen9GraphicsDevice>::make(allocator_, allocator_, driver_, memory_, display_);
  if (!device_ || !device_->initialize()) {
    device_ = UniquePointer<Gen9GraphicsDevice>();
    driver_.shutdown();
    return false;
  }
  return true;
}

}  // namespace graphics
