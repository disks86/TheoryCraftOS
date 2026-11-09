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

// The key of a fixed-function program and what the program knows about itself (see FixedFunction.cpp).
#pragma once
#include <stdint.h>
#include "OpenGl/GlState.hpp"

namespace opengl {

enum : uint8_t { kBaseAlpha, kBaseLuminance, kBaseLuminanceAlpha, kBaseIntensity, kBaseRgb, kBaseRgba };
enum : uint8_t { kDimNone, kDimFixed1D, kDimFixed2D, kDimFixed3D, kDimFixedCube, kDimFixedRect };
enum : uint8_t { kEnvModulate, kEnvReplace, kEnvDecal, kEnvBlend, kEnvAdd, kEnvCombine };
enum : uint8_t { kGenOff, kGenObject, kGenEye, kGenSphere, kGenNormal, kGenReflection };

struct FixedKey {
  uint8_t lights = 0, light_positional = 0, light_spot = 0, clip_mask = 0;
  bool lighting = false, two_side = false, local_viewer = false, separate_specular = false, color_material = false, color_sum = false;
  uint8_t color_material_face = 0, color_material_mode = 0;
  bool fog = false, fog_from_coord = false;
  uint8_t fog_mode = 0;
  bool normalize = false, rescale = false, point_attenuated = false, needs_reflection = false, needs_normal = false, needs_eye = false;
  uint8_t tex_needs_coord = 0, tex_matrix = 0, tex_shadow = 0, tex_bias = 0;
  uint8_t tex_dim[kMaxTexUnits] = {}, tex_base[kMaxTexUnits] = {}, tex_mode[kMaxTexUnits] = {};
  uint16_t texgen[kMaxTexUnits] = {};
  struct Combine {
    uint8_t mode_rgb = 0, mode_a = 0;
    uint8_t src_rgb[3] = {}, src_a[3] = {}, op_rgb[3] = {}, op_a[3] = {};
  } combine[kMaxTexUnits];
  // (All members are bytes in a fixed order with no gaps but padding that value-initialisation zeroes, compared member by member through this.)
  bool operator==(const FixedKey& o) const { return __builtin_memcmp(this, &o, sizeof(FixedKey)) == 0; }
};

struct FixedProgram {
  FixedKey key;
  uint32_t program = 0;
  uint64_t generation = 0;
  uint32_t flat_first = 0, flat_count = 0;
  int32_t clip_first = -1;
  int32_t u_mvp = -1, u_mv = -1, u_nm = -1, u_nscale = -1, u_ps = -1, u_pa = -1, u_pr = -1, u_model = -1, u_fc = -1, u_fp = -1;
  int32_t u_ma[2], u_md[2], u_ms[2], u_me[2], u_shin[2];
  int32_t u_lA[kMaxLights], u_lD[kMaxLights], u_lS[kMaxLights], u_lp[kMaxLights], u_la[kMaxLights], u_ld[kMaxLights], u_lc[kMaxLights], u_le[kMaxLights];
  int32_t u_cp[kMaxClipPlanes];
  int32_t u_tm[kMaxTexUnits], u_s[kMaxTexUnits], u_ec[kMaxTexUnits], u_es[kMaxTexUnits], u_tb[kMaxTexUnits], u_rs[kMaxTexUnits], u_gp[kMaxTexUnits][4];
};

bool mat_is_identity(const MatrixStack& s);

}  // namespace opengl
