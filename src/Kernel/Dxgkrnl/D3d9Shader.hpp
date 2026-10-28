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

// Direct3D 9 shader bytecode (the vs_1_1 / vs_2_0 / vs_3_0 and ps_2_0 / ps_3_0 token streams a D3D9 runtime hands its driver) for the devices that cannot take it as it is.
//
// The software device runs shaders as native functions, so the bytecode is decoded and checked once (D3d9Program::compile) and then interpreted per vertex / pixel;
// D3d9Binding gives such a program a native function to be called as (a device's shader function has no context pointer, so each program takes one of a fixed number
// of pre-built trampolines that find it again).
//
// What maps to what (the contract with the user-mode driver):
//  - Vertex inputs: register vN is vertex attribute location N (N < 4). Outputs go to the varyings by meaning: oD0 / oD1 (or a COLOR0 / COLOR1 output of a 3.0 shader)
//    to varying 0 / 1, oT0 / oT1 (TEXCOORD0 / TEXCOORD1) to varying 2 / 3, so a pixel shader's v0 / v1 / t0 / t1 pick them up. Clip space is Direct3D's (+Y up, depth 0..1);
//    the program flips Y for the device (Vulkan's +Y down), so a pipeline made for these shaders uses FrontFace::Clockwise for Direct3D's default winding.
//  - Constants: c0..c7 are the push constants (8 x float4, set with PushConstants); `def` constants of the shader are its own. A shader using another constant register
//    (other than through a relative address, which reads 0 there) is refused.
//  - Pixel shaders write oC0; samplers s0 / s1 are texture units 0 / 1 (2D only).
//  - Supported: the arithmetic, dot-product, matrix, comparison and sincos / pow / exp / log / lit / dst / lrp / cmp / dp2add instructions, texld (with the project flag) and
//    texkill, if / else / endif (on a def'd boolean or with ifc), relative addressing of constants with a0 in vertex shaders, write masks, swizzles, source modifiers
//    (negate, bias, sign, complement, x2, abs), result saturate and shift. Not supported (refused when the shader is made): loops, calls, rep, predication, integer and
//    boolean registers other than defb, ps_1_x, depth output, point size / fog outputs, cube and volume samplers.
#pragma once
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace dxgk {

// Which of the two fixed pixel kernels of a device without a shader compiler (Intel Gen9) a pixel shader is exactly: the interpolated varying as the colour, or one texture sample at
// the interpolated varying (first two components) with unit 0.
enum class FragmentKernel : uint8_t { None, Colored, Textured };

// Whether a vertex shader is exactly the transform the fixed vertex kernel of such a device does (the built-in coloured vertex shader: clip = M * (position, 1), the second attribute
// to a varying), and with the matrix as columns (c0..c3 are the columns: what the built-in shader takes) or as rows (c0..c3 are what dp4 takes: the transpose).
enum class VertexKernel : uint8_t { None, MatrixColumns, MatrixRows };

class D3d9Program {
 public:
  static constexpr uint32_t kMaxTokens = 8192;
  static constexpr uint32_t kMaxInstructions = 1024;
  static constexpr uint32_t kMaxSteps = 1u << 20;   // instructions one run may execute
  static constexpr uint32_t kPushConstantRegisters = 8;
  static constexpr uint32_t kMaxConstantRegisters = 256;   // 4096 bytes: a program made from DXBC may declare constant buffers up to this much in all

  // Decodes and checks the token stream (little-endian words). kInvalidParameter for a malformed stream, kNotSupported for one that is fine but uses something not supported.
  static NtStatus compile(allocators::IAllocator& allocator, ConstByteView tokens, UniquePointer<D3d9Program>* out);

  // The same for a DXBC container (Shader Model 4.0 / 4.1 / 5.0 vertex and pixel shaders), see D3d9Dxbc.cpp for what maps to what.
  // Pixel shaders of Shader Model 1 (ps_1_0 .. ps_1_4), see D3d9Ps1.cpp; compile() hands them over.
  static NtStatus compile_ps1(allocators::IAllocator& allocator, ConstByteView tokens, UniquePointer<D3d9Program>* out);
  static NtStatus compile_dxbc(allocators::IAllocator& allocator, ConstByteView container, UniquePointer<D3d9Program>* out);

  graphics::ShaderStage stage() const { return stage_; }
  // How many bytes of push constants the program reads (a Direct3D 9 program: eight registers; a DXBC one: its constant buffers laid end to end in slot order). A device whose
  // limit is lower cannot run it.
  uint32_t constant_bytes() const { return constant_registers_ * 16; }
  bool uses_textures() const { return uses_textures_; }

  // Runs the program. `attributes` are the vertex attributes by location, `constants` the 128-byte push-constant block.
  void run_vertex(const graphics::Vec4* attributes, const uint8_t* constants, graphics::ShaderIo* out) const;
  // `textures` may be null when uses_textures() is false. False: the pixel was killed.
  bool run_fragment(const graphics::Vec4* varyings, const uint8_t* constants, const graphics::ITextureSet* textures, graphics::Vec4* color) const;

  // The same program as TGSI text for a device that runs shaders on a GPU (virgl), following ShaderModuleDesc::tgsi: vertex inputs IN[n] are attribute locations, varyings
  // GENERIC[0..3] are the four of the mapping above, CONST[0..7] the push constants and the shader's `def`s immediates, the position gets the same Y flip as run_vertex. kNotSupported for
  // what the text cannot say (see D3d9Tgsi.cpp), kNoMemory.
  NtStatus emit_tgsi(String* out) const;

  // For a vertex shader: whether it is the matrix transform of the fixed vertex kernel, found by running it on test vectors against the built-in shader. `colour_varying` is the varying
  // the colour attribute goes to.
  VertexKernel classify_vertex(uint8_t* colour_varying) const;
  // For a pixel shader: the fixed kernel it is, and the varying that kernel reads (see D3d9Devices.cpp).
  FragmentKernel classify_fragment(uint8_t* varying) const;
  // The shader as the tokens the VMware SVGA3D device wants (the vs_2_0 / ps_2_0 token stream the host runs): from the Direct3D 9 stream the program was made of. A vertex shader gets
  // the device's declarations (input 0 position, 1 colour, the rest texture coordinates) and its position multiplied by the constant c31 (1, -1, 1, 1) the device supplies, so that
  // +Y comes out right. kNotSupported for what the device cannot run: other models than 2.0 / 3.0, a vertex shader with no temporary left for the position, a `def c31`, a program
  // made from DXBC (write it with emit_d3d9_tokens first).
  NtStatus emit_svga_tokens(ConstByteView original, List<uint32_t>* out) const;
  // A program made from DXBC as a Direct3D 9 token stream (vs_2_0 / ps_2_0, or the 3.0 versions when it branches on a register), in the contract above - see D3d9Emit.cpp.
  // kNotSupported for a Direct3D 9 program, one that needs more temporaries or constants than the model has, or uses what cannot be written.
  NtStatus emit_d3d9_tokens(List<uint32_t>* out) const;

  struct Instruction;
  explicit D3d9Program(allocators::IAllocator& allocator);
  ~D3d9Program();

 private:
  friend class TgsiEmitter;
  friend class D3d9Writer;
  struct State;
  void execute(State& s) const;

  allocators::IAllocator& allocator_;
  graphics::ShaderStage stage_ = graphics::ShaderStage::Vertex;
  uint32_t major_ = 0, minor_ = 0;
  bool uses_textures_ = false;
  uint32_t constant_registers_ = kPushConstantRegisters;
  List<Instruction> code_;
  struct Def {
    uint16_t number;
    float value[4];
  };
  List<Def> defs_;
  struct DefBool {
    uint16_t number;
    bool value;
  };
  List<DefBool> bools_;
};

// Gives a compiled program a native function to be called as. The slots are a fixed number shared by the whole system.
class D3d9Binding {
 public:
  D3d9Binding() = default;
  ~D3d9Binding() { release(); }
  D3d9Binding(const D3d9Binding&) = delete;
  D3d9Binding& operator=(const D3d9Binding&) = delete;

  // Takes the program and fills in the function of `desc` (vertex_function, fragment_function or textured_fragment_function) and its stage. kInsufficientResources when
  // all slots are used.
  NtStatus bind(UniquePointer<D3d9Program> program, graphics::ShaderModuleDesc* desc);
  void release();

 private:
  UniquePointer<D3d9Program> program_;
  int32_t slot_ = -1;
  uint8_t kind_ = 0;
};

}  // namespace dxgk
