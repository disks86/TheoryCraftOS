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


// An emulator of GCN3 (GFX8) shader waves: the instructions GcnIsa.hpp assembles, executed 64 lanes at a time with the execution mask, so that the output of the shader compiler can
// be run (and tested) without the card. The environment supplies what the hardware gets from outside the wave: memory (constants, vertex buffers), the interpolation parameters of the
// pixel shader's inputs, texture sampling, and takes the exports.
//
// Semantics follow the AMD GCN3 ISA document for the subset: single precision floats flush denormals (the graphics default of the MODE register), the minimum and maximum of a NaN and
// a number is the number, v_rcp / v_rsq / v_sqrt / v_exp / v_log are the exact functions (the hardware's are accurate to one ulp), v_sin / v_cos take the angle in turns (1.0 is 2 pi).
#pragma once

#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnIsa.hpp"

namespace gcn {

constexpr uint32_t kWaveSize = 64;

class IEnvironment {
 public:
  // Memory by GPU address: constants, buffers (the descriptors say where). False if the address is not mapped (the wave stops with Status::MemoryFault).
  virtual bool load32(uint64_t address, uint32_t* value) = 0;
  virtual bool store32(uint64_t address, uint32_t value) = 0;
  // The parameters of pixel shader input `attr`, channel `chan` for a lane: v_interp_p1 gives p10 * i + p0 and v_interp_p2 adds p20 * j.
  virtual void interpolation(uint32_t attr, uint32_t chan, uint32_t lane, float* p0, float* p10, float* p20) = 0;
  // image_sample and friends for one lane: the 8 dword image descriptor, the 4 dword sampler, the coordinates (the VGPRs from vaddr on, as many as the instruction reads) and the
  // lod bias / lod where the opcode has one. Gives the four components of the result.
  virtual void sample(Mimg op, const uint32_t image[8], const uint32_t sampler[4], const float* coords, uint32_t coord_count, float result[4]) = 0;
  // An export: `target` as in the instruction, `enable` the component mask, `values[lane][4]` the exported registers (for a compressed export two packed halves in [0] and [1]), and the
  // lanes (bit n) that export. `done` marks the last export.
  virtual void export_data(uint32_t target, uint32_t enable, bool compressed, const uint32_t values[kWaveSize][4], uint64_t lanes, bool done) = 0;

 protected:
  IEnvironment() = default;
  ~IEnvironment() = default;
};

enum class Status : uint8_t { Running, Done, BadInstruction, MemoryFault, StepLimit, BadBranch };

class Wave {
 public:
  Wave();
  // The state a shader starts from: SGPRs and VGPRs zero, EXEC = the first `lanes` lanes.
  void reset(uint32_t lanes);

  uint32_t sgpr[128] = {};                  // 0..101 the general ones; 106 / 107 VCC, 124 M0, 126 / 127 EXEC are in the members below
  uint32_t vgpr[256][kWaveSize] = {};
  uint64_t vcc = 0, exec = 0;
  uint32_t m0 = 0;
  bool scc = false;
  uint32_t pc = 0;                          // in dwords
  uint64_t steps = 0;
  Status status = Status::Running;

  // Runs from `pc` until s_endpgm or a problem; `max_steps` bounds the instructions executed.
  Status run(ArrayView<const uint32_t> code, IEnvironment& env, uint64_t max_steps = 1000000);
  // One instruction.
  Status step(ArrayView<const uint32_t> code, IEnvironment& env);

  float vgpr_f(uint32_t reg, uint32_t lane) const;
  void set_vgpr_f(uint32_t reg, uint32_t lane, float value);

 private:
  Status fail(Status s) { status = s; return s; }
  bool src32(uint32_t code, uint32_t literal, uint32_t lane, uint32_t* out) const;      // a 9 bit source for one lane
  bool src64(uint32_t code, uint32_t literal, uint64_t* out) const;                    // a scalar source as a 64 bit value (a pair, VCC, EXEC, a constant)
  void put_sdst(uint32_t code, uint32_t value);
  void put_sdst64(uint32_t code, uint64_t value);
  uint32_t get_sreg(uint32_t index) const;
  Status exec_sop1(uint32_t w, uint32_t literal);
  Status exec_sop2(uint32_t w, uint32_t literal);
  Status exec_sopk(uint32_t w);
  Status exec_sopc(uint32_t w, uint32_t literal);
  Status exec_sopp(uint32_t w);
  Status exec_smem(uint32_t w0, uint32_t w1, IEnvironment& env);
  Status exec_valu(uint32_t w0, uint32_t w1, uint32_t literal);
  Status exec_vintrp(uint32_t w, IEnvironment& env);
  Status exec_exp(uint32_t w0, uint32_t w1, IEnvironment& env);
  Status exec_mubuf(uint32_t w0, uint32_t w1, IEnvironment& env);
  Status exec_mimg(uint32_t w0, uint32_t w1, IEnvironment& env);
};

}  // namespace gcn
