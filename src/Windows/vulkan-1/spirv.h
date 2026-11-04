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

/* The SPIR-V to DXBC translator of vulkan-1.dll (spirv.inc is the code). A Vulkan shader module arrives as SPIR-V; the kernel's Direct3D layer (src/Kernel/Dxgkrnl) takes Shader
 * Model 4.0 DXBC for every GPU it drives, so the driver translates once and the same shader runs on the CPU renderer, virgl, SVGA3D, Intel Gen9, V3D and Vega.
 *
 * Two passes, because the numbers a shader's resources get (constant buffer slots, texture units) are those of the whole pipeline, not of one shader:
 *   spv_open     parse the module
 *   spv_reflect  find the entry point and the resources it uses (spv_resource_count / spv_resource), the vertex inputs and the outputs
 *   -- the caller numbers the resources of all the stages of a pipeline (SpvResource::slot, ::cb_vec4) --
 *   spv_emit     translate the entry point into a DXBC container
 *   spv_close
 * Self-contained C: no libc; the allocator is the caller's. */
#ifndef TCOS_SPIRV_H
#define TCOS_SPIRV_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpvAllocator {
  void *(*alloc)(void *user, unsigned long long bytes);   /* zeroed memory, NULL when it ran out */
  void (*release)(void *user, void *memory);
  void *user;
} SpvAllocator;

enum { SPV_STAGE_VERTEX = 0, SPV_STAGE_FRAGMENT = 4, SPV_STAGE_COMPUTE = 5 };   /* SPIR-V execution models 0, 4, 5 */

enum { SPV_RES_UNIFORM = 1, SPV_RES_PUSH = 2, SPV_RES_IMAGE = 3, SPV_RES_SAMPLER = 4, SPV_RES_STORAGE = 5, SPV_RES_COMBINED = 6 };
enum { SPV_DIM_2D = 0, SPV_DIM_3D = 1, SPV_DIM_CUBE = 2, SPV_DIM_2D_ARRAY = 3 };

typedef struct SpvResource {
  uint32_t kind;          /* SPV_RES_* */
  uint32_t set, binding;
  uint32_t array_size;    /* elements (1 for a single resource) */
  uint32_t bytes;         /* uniform / push constant blocks: the size of the block in bytes */
  uint32_t dim;           /* images: SPV_DIM_* */
  uint32_t depth;         /* images: sampled with a depth comparison somewhere */
  uint32_t slot;          /* set by the caller: constant buffer slot (uniform, push constants), texture unit (image, combined; a sampler has the unit of the image it is used with), storage slot */
  uint32_t cb_vec4;       /* set by the caller: size of the constant buffer in 16-byte registers (at least ceil(bytes / 16)); the same for every stage of a pipeline */
  uint32_t used;
  uint32_t sampler_slot;  /* set by the translator: for a SPV_RES_IMAGE sampled with a separate sampler, the resource index of that sampler (or ~0) */
} SpvResource;

typedef struct SpvModule SpvModule;

SpvModule *spv_open(const uint32_t *words, uint32_t count, const SpvAllocator *allocator);   /* NULL: not a SPIR-V module or no memory */
/* 0 or an error; spv_error says what. `spec_ids` / `spec_values` / `spec_count`: values for specialization constants (may be 0). */
int spv_reflect(SpvModule *m, uint32_t stage, const char *entry_name, const uint32_t *spec_ids, const uint32_t *spec_values, uint32_t spec_count);
uint32_t spv_resource_count(const SpvModule *m);
SpvResource *spv_resource(SpvModule *m, uint32_t index);
uint32_t spv_input_mask(const SpvModule *m);     /* vertex stage: bit n set: location n is read; fragment: the varyings (locations) read */
uint32_t spv_output_mask(const SpvModule *m);    /* vertex: varyings (locations) written; fragment: colour attachments written */
uint32_t spv_uses_frag_coord(const SpvModule *m);
/* Every constant buffer slot of the pipeline must be declared in every stage, whether the stage uses it or not (the kernel lays the buffers end to end by slot): the size in 16-byte registers. */
void spv_declare_cb(SpvModule *m, uint32_t slot, uint32_t vec4);
/* The DXBC container; *out is owned by the module (valid until spv_close). */
int spv_emit(SpvModule *m, const uint8_t **out, uint32_t *size);
const char *spv_error(const SpvModule *m);
void spv_close(SpvModule *m);

#ifdef __cplusplus
}
#endif
#endif
