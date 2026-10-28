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

// What the two devices that do not take a shader as a function or as TGSI can be given from a Direct3D 9 program.
//
//  - Intel Gen9 has no compiler for its execution units: it runs its vertex stage on the CPU (so it takes the interpreter's vertex function) and a pixel stage that is one of two fixed
//    kernels. classify_fragment() says whether a pixel shader is exactly one of them (`mov oC0, v0`, or a texture sample at a varying written to oC0).
//  - VMware SVGA3D takes the Direct3D 9 token stream itself, made for its conventions: emit_svga_tokens() rewrites a shader of the D3D9 contract into them (a program made from
//    DXBC is written as such a stream first, D3d9Emit.cpp).
#include "Kernel/Dxgkrnl/D3d9Decoded.hpp"
#include "Utility/ByteStream.hpp"

namespace dxgk {

namespace {

uint8_t reg_type_of(uint32_t token) { return static_cast<uint8_t>(((token >> 28) & 7) | ((token >> 8) & 0x18)); }

bool identity(const uint8_t s[4]) { return s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3; }

// A source that reads a varying as it is (`xy_only`: only the first two components matter, as for a texture coordinate).
bool plain_varying(const D3d9Program::Instruction::Src& s, bool xy_only = false) {
  return (s.type == kInput || s.type == kAddrTex) && s.slot >= 0 && !s.relative && s.modifier == kModNone && (xy_only ? s.swizzle[0] == 0 && s.swizzle[1] == 1 : identity(s.swizzle));
}

bool plain_sample(const D3d9Program::Instruction& a) {
  return a.op == kTexLd && a.dst.mask == 0xF && !a.dst.saturate && a.dst.shift == 0 && (a.control & 1) == 0 && plain_varying(a.src[0], true) && a.src[1].type == kSampler &&
         a.src[1].number == 0 && identity(a.tex_swizzle);
}

bool plain_color_write(const D3d9Program::Instruction::Dst& d) { return d.type == kColorOut && d.mask == 0xF && !d.saturate && d.shift == 0; }

}  // namespace

FragmentKernel D3d9Program::classify_fragment(uint8_t* varying) const {
  if (stage_ != graphics::ShaderStage::Fragment) return FragmentKernel::None;
  if (code_.count() == 1) {
    const Instruction& a = code_[0];
    if (a.op == kMov && plain_color_write(a.dst) && plain_varying(a.src[0])) {
      *varying = static_cast<uint8_t>(a.src[0].slot);
      return FragmentKernel::Colored;
    }
    if (plain_sample(a) && plain_color_write(a.dst)) {   // (sampled straight into the colour output)
      *varying = static_cast<uint8_t>(a.src[0].slot);
      return FragmentKernel::Textured;
    }
    return FragmentKernel::None;
  }
  if (code_.count() == 2) {
    const Instruction &a = code_[0], &b = code_[1];
    const bool sample = plain_sample(a) && a.dst.type == kTemp;
    const bool copy = b.op == kMov && plain_color_write(b.dst) && b.src[0].type == kTemp && b.src[0].number == a.dst.number && b.src[0].modifier == kModNone && !b.src[0].relative &&
                      identity(b.src[0].swizzle);
    if (sample && copy) {
      *varying = static_cast<uint8_t>(a.src[0].slot);
      return FragmentKernel::Textured;
    }
  }
  return FragmentKernel::None;
}

NtStatus D3d9Program::emit_svga_tokens(ConstByteView original, List<uint32_t>* out) const {
  const bool vertex = stage_ == graphics::ShaderStage::Vertex;
  if (!(((major_ == 2 || major_ == 3) && minor_ == 0) || (major_ == 1 && minor_ == 1)) || original.size() < 8 || original.size() % 4) return status::kNotSupported;   // (a DXBC program has major 4 or 5)
  uint32_t temporaries = 0;
  for (const Instruction& ins : code_.view()) {
    if (ins.dst.type == kTemp && ins.dst.number + 1u > temporaries && d3d9_op_writes_register(ins.op)) temporaries = ins.dst.number + 1u;
    for (uint32_t k = 0; k < ins.src_count; ++k) {
      const Instruction::Src& s = ins.src[k];
      if (s.type == kTemp && s.number + 1u > temporaries) temporaries = s.number + 1u;
    }
  }
  if (vertex) {
    if (temporaries >= (major_ == 3 ? 32u : 12u)) return status::kNotSupported;   // (vs_2_0 has r0..r11, vs_3_0 r0..r31)
    for (const Def& d : defs_.view())
      if (d.number == 31) return status::kNotSupported;
  }
  const uint32_t position_temp = temporaries;
  int32_t position_register = -1;   // Shader Model 3: the output register declared as the position

  ByteReader in(original);
  auto put = [&](uint32_t v) { return out->push_back(v); };
  const uint32_t version = in.le32();
  if (!put(version)) return status::kNoMemory;
  while (!in.at_end()) {
    const uint32_t token = in.le32();
    if (!in.ok()) return status::kInvalidParameter;
    const uint16_t op = static_cast<uint16_t>(token & 0xFFFF);
    if (token == 0x0000FFFF) {
      if (vertex) {
        // mul oPos, rT, c31
        if (major_ == 3 && position_register < 0) return status::kNotSupported;
        // (oPos in Shader Model 2, the declared output register in 3)
        const uint32_t destination = major_ == 3 ? 0xE00F0000u | static_cast<uint32_t>(position_register) : 0xC00F0000u;
        const uint32_t extra[] = {kMul | (major_ < 2 ? 0u : 3u << 24), destination, 0x80E40000u | position_temp, 0xA0E4001Fu};
        for (uint32_t w : extra)
          if (!put(w)) return status::kNoMemory;
      }
      if (!put(token)) return status::kNoMemory;
      return status::kSuccess;
    }
    uint32_t words = (token >> 24) & 0xF;
    if (op == kComment) {
      words = (token >> 16) & 0x7FFF;
    } else if (major_ < 2) {
      // (Shader Model 1 instructions say nothing of their length: it is the opcode's)
      const int n = d3d9_op_parameter_tokens(op);
      if (n < 0) return status::kInvalidParameter;
      words = static_cast<uint32_t>(n);
    }
    if (!put(token)) return status::kNoMemory;
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t w = in.le32();
      if (!in.ok()) return status::kInvalidParameter;
      if (vertex && op == kDcl && i == 0) {
        // The declaration of an input says what it is: the device's vertex declaration names location 0 the position, 1 the colour, 2.. texture coordinates.
        // (the register is the next word; a sampler or output declaration has no such meaning)
        ByteReader peek(in.rest());
        const uint32_t reg = peek.le32();
        if (peek.ok() && major_ == 3 && reg_type_of(reg) == kTexCrdOut && (w & 0x1F) == kUsagePosition && ((w >> 16) & 0xF) == 0) position_register = static_cast<int32_t>(reg & 0x7FF);
        if (peek.ok() && reg_type_of(reg) == kInput) {
          const uint32_t location = reg & 0x7FF;
          const uint32_t usage = location == 0 ? kUsagePosition : location == 1 ? kUsageColor : kUsageTexCoord, index = location >= 2 ? location - 2 : 0;
          w = (w & ~0x000F001Fu) | usage | (index << 16);
        }
      } else if (vertex && i == 0 && d3d9_op_writes_register(op) && op != kDcl &&
                 (major_ == 3 ? reg_type_of(w) == kTexCrdOut && position_register >= 0 && (w & 0x7FF) == static_cast<uint32_t>(position_register) : reg_type_of(w) == kRastOut && (w & 0x7FF) == 0)) {
        w = (w & ~(0x70000000u | 0x00001800u | 0x7FFu)) | position_temp;   // oPos becomes the temporary
      }
      if (!put(w)) return status::kNoMemory;
    }
  }
  return status::kInvalidParameter;   // (no end token)
}

}  // namespace dxgk
