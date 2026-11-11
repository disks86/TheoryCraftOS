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

#include "Desktop/CubeSpirv.hpp"
#include "Desktop/CubeShaders/CubeSpirv.inc"
#include "Vulkan/spirv.h"

namespace desktop {

namespace {

// The translator's allocator over the kernel's: zeroed memory, released without a size.
void* spirv_allocate(void* user, unsigned long long bytes) {
  auto* allocator = static_cast<allocators::IAllocator*>(user);
  if (!bytes) bytes = 1;
  auto* memory = static_cast<uint8_t*>(allocator->allocate(bytes));
  if (memory) ByteView(memory, bytes).fill(0);
  return memory;
}

void spirv_release(void* user, void* memory) {
  if (memory) static_cast<allocators::IAllocator*>(user)->deallocate(memory);
}

// Closes the module however the translation ends.
struct ModuleGuard {
  SpvModule* module;
  ~ModuleGuard() {
    if (module) spv_close(module);
  }
};

}  // namespace

ArrayView<const uint32_t> cube_spirv(bool textured, SpirvStage stage) {
  if (stage == SpirvStage::Vertex) return textured ? ArrayView<const uint32_t>(kTexturedVertexSpirv) : ArrayView<const uint32_t>(kColoredVertexSpirv);
  return textured ? ArrayView<const uint32_t>(kTexturedFragmentSpirv) : ArrayView<const uint32_t>(kColoredFragmentSpirv);
}

bool translate_spirv(allocators::IAllocator& allocator, ArrayView<const uint32_t> words, SpirvStage stage, uint32_t push_bytes, ByteBuffer& dxbc, SpirvTranslation* info,
                     const char** error) {
  dxbc.clear();
  if (error) *error = "";
  if (words.size() > 0xFFFFFFFFu) return false;
  const SpvAllocator spv_allocator = {spirv_allocate, spirv_release, &allocator};
  ModuleGuard guard{spv_open(words.data(), static_cast<uint32_t>(words.size()), &spv_allocator)};
  if (!guard.module) {
    if (error) *error = "not a SPIR-V module, or no memory";
    return false;
  }
  if (spv_reflect(guard.module, stage == SpirvStage::Vertex ? SPV_STAGE_VERTEX : SPV_STAGE_FRAGMENT, "main", nullptr, nullptr, 0)) {
    if (error) *error = spv_error(guard.module);
    return false;
  }
  // (as vulkan-1.dll numbers them: the push constants are slot 0 of every stage, so that the kernel finds the same layout whichever stage reads them)
  const uint32_t push_vec4 = (push_bytes + 15) / 16;
  uint32_t units = 0;
  for (uint32_t i = 0; i < spv_resource_count(guard.module); ++i) {
    SpvResource* r = spv_resource(guard.module, i);
    if (r->kind == SPV_RES_PUSH) {
      if (r->bytes > push_bytes) {
        if (error) *error = "a push constant block larger than the renderer sends";
        return false;
      }
      r->slot = 0;
      r->cb_vec4 = push_vec4;
    } else if (r->kind == SPV_RES_COMBINED || r->kind == SPV_RES_IMAGE) {
      r->slot = units++;
    } else {
      if (error) *error = "a resource the cube does not provide";
      return false;
    }
  }
  if (push_vec4) spv_declare_cb(guard.module, 0, push_vec4);
  const uint8_t* out = nullptr;
  uint32_t size = 0;
  if (spv_emit(guard.module, &out, &size) || !out) {
    if (error) *error = spv_error(guard.module);
    return false;
  }
  if (!dxbc.append(ConstByteView(out, size))) return false;
  if (info) {
    info->inputs = spv_input_mask(guard.module);
    info->outputs = spv_output_mask(guard.module);
    info->texture_units = units;
  }
  return true;
}

}  // namespace desktop
