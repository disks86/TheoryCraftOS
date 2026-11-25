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

// Vertex formats the virgl host cannot fetch the way Direct3D reads them, made right in the vertex shader's TGSI text.
//
// virglrenderer turns a vertex element into an OpenGL vertex attribute, and OpenGL has no 10/10/10/2 integer attribute (R10G10B10A2_UINT is refused), and a
// B8G8R8X8 element is read as B8G8R8A8 (the fourth byte is the alpha, where Direct3D gives 1). The pipeline feeds such an input as something the host takes - the
// whole 32-bit word as R32_UINT, the bytes as B8G8R8A8 - and the vertex shader it binds is a copy of the module's text that, before its first instruction, makes
// the register Direct3D's value in a temporary of its own (unpacking the four fields, or setting w to 1) and reads that temporary wherever it read the input.
#pragma once

#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace graphics::virgl_patch {

enum class InputFix : uint8_t {
  None,
  UnpackRgb10A2Uint,   // the input is the 32-bit word (R32_UINT): it becomes (bits 0-9, 10-19, 20-29, 30-31) as unsigned integers
  ForceW1,             // the input's w becomes the float 1
};

namespace detail {
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_word(char c) { return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
// The decimal number at `at` of `text` (and where it ends); false when there is none or it is too large.
inline bool parse_number(StringView text, uint64_t at, uint32_t* value, uint64_t* end) {
  uint64_t v = 0, i = at;
  while (i < text.size() && is_digit(text[i])) {
    v = v * 10 + static_cast<uint64_t>(text[i] - '0');
    if (v > 0xFFFFFFu) return false;
    ++i;
  }
  if (i == at) return false;
  *value = static_cast<uint32_t>(v);
  *end = i;
  return true;
}
inline bool starts_with(StringView text, uint64_t at, const char* prefix) {
  for (uint64_t k = 0; prefix[k]; ++k)
    if (at + k >= text.size() || text[at + k] != prefix[k]) return false;
  return true;
}
// An instruction line: spaces, a number, a colon.
inline bool is_instruction_line(StringView line) {
  uint64_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  uint32_t label = 0;
  uint64_t end = 0;
  return parse_number(line, i, &label, &end) && end < line.size() && line[end] == ':';
}
}  // namespace detail

// Writes into `out` the text `tgsi` of a vertex shader with the inputs fixed as `fixes` says (fixes[n] for IN[n]). False when the text is not one this can change
// (an input read through an address register, no declarations before the first instruction, numbers out of range) or memory ran out.
[[nodiscard]] inline bool patch_vertex_inputs(StringView tgsi, ArrayView<const InputFix> fixes, String* out) {
  using namespace detail;
  out->clear();
  // The header (declarations and immediates) ends at the first instruction line.
  uint64_t body = tgsi.size();
  bool found_body = false;
  uint32_t temps = 0, immediates = 0;   // (the temporaries declared, counting from 0, and the immediates)
  uint64_t temp_decl = tgsi.size(), temp_decl_end = 0;
  for (uint64_t at = 0; at < tgsi.size();) {
    uint64_t end = at;
    while (end < tgsi.size() && tgsi[end] != '\n') ++end;
    const StringView line = tgsi.substr(at, end - at);
    if (is_instruction_line(line)) {
      body = at;
      found_body = true;
      break;
    }
    if (starts_with(tgsi, at, "IMM[")) ++immediates;
    if (starts_with(tgsi, at, "DCL TEMP[")) {
      // "DCL TEMP[a..b]" or "DCL TEMP[a]": the temporaries are 0 .. b
      uint32_t first = 0, last = 0;
      uint64_t e = 0;
      if (!parse_number(tgsi, at + 9, &first, &e)) return false;
      last = first;
      if (starts_with(tgsi, e, "..") && !parse_number(tgsi, e + 2, &last, &e)) return false;
      if (e >= tgsi.size() || tgsi[e] != ']') return false;
      if (last + 1 > temps) temps = last + 1;
      if (temp_decl == tgsi.size()) temp_decl = at, temp_decl_end = end;
    }
    at = end + 1;
  }
  if (!found_body) return false;
  // Each fixed input gets the temporary temps + (its number among the fixed ones).
  uint32_t temp_of[64];
  uint32_t fixed = 0;
  bool unpacks = false, force_w = false;
  if (fixes.size() > 64) return false;
  for (uint64_t k = 0; k < fixes.size(); ++k) {
    temp_of[k] = 0;
    if (fixes[k] == InputFix::None) continue;
    temp_of[k] = temps + fixed++;
    (fixes[k] == InputFix::UnpackRgb10A2Uint ? unpacks : force_w) = true;
  }
  if (fixed == 0) return out->assign(tgsi);
  const uint32_t imm_shift = immediates, imm_mask = immediates + 1, imm_one = immediates + (unpacks ? 2 : 0);
  bool ok = true;
  // The header, with the temporaries' declaration covering the new ones.
  if (temp_decl != tgsi.size()) {
    ok = ok && out->append(tgsi.substr(0, temp_decl)) && out->append("DCL TEMP[0..") && out->append_uint(temps + fixed - 1) && out->append("]") &&
         out->append(tgsi.substr(temp_decl_end, body - temp_decl_end));
  } else {
    ok = ok && out->append(tgsi.substr(0, body)) && out->append("DCL TEMP[0..") && out->append_uint(fixed - 1) && out->append("]\n");
  }
  // (immediates as their bits: the shift counts, the field masks, and the bits of 1.0f)
  if (unpacks) {
    ok = ok && out->append("IMM[") && out->append_uint(imm_shift) && out->append("] UINT32 {0, 10, 20, 30}\n");
    ok = ok && out->append("IMM[") && out->append_uint(imm_mask) && out->append("] UINT32 {1023, 1023, 1023, 3}\n");
  }
  if (force_w) ok = ok && out->append("IMM[") && out->append_uint(imm_one) && out->append("] UINT32 {0, 0, 0, 1065353216}\n");
  // The new instructions (TGSI's instruction labels are optional), then the body reading the temporaries.
  for (uint64_t k = 0; k < fixes.size(); ++k) {
    if (fixes[k] == InputFix::UnpackRgb10A2Uint) {
      ok = ok && out->append("  USHR TEMP[") && out->append_uint(temp_of[k]) && out->append("], IN[") && out->append_uint(k) && out->append("].xxxx, IMM[") && out->append_uint(imm_shift) &&
           out->append("]\n  AND TEMP[") && out->append_uint(temp_of[k]) && out->append("], TEMP[") && out->append_uint(temp_of[k]) && out->append("], IMM[") && out->append_uint(imm_mask) &&
           out->append("]\n");
    } else if (fixes[k] == InputFix::ForceW1) {
      ok = ok && out->append("  MOV TEMP[") && out->append_uint(temp_of[k]) && out->append("], IN[") && out->append_uint(k) && out->append("]\n  MOV TEMP[") && out->append_uint(temp_of[k]) &&
           out->append("].w, IMM[") && out->append_uint(imm_one) && out->append("].wwww\n");
    }
  }
  uint64_t copied = body;
  for (uint64_t at = body; ok && at < tgsi.size(); ++at) {
    if (!starts_with(tgsi, at, "IN[") || (at > 0 && is_word(tgsi[at - 1]))) continue;
    uint32_t n = 0;
    uint64_t e = 0;
    if (!parse_number(tgsi, at + 3, &n, &e) || e >= tgsi.size() || tgsi[e] != ']') return false;   // (an input by an address register: which one is not known)
    if (n >= fixes.size() || fixes[n] == InputFix::None) continue;
    ok = ok && out->append(tgsi.substr(copied, at - copied)) && out->append("TEMP[") && out->append_uint(temp_of[n]) && out->append("]");
    copied = e + 1;
    at = e;
  }
  ok = ok && out->append(tgsi.substr(copied, tgsi.size() - copied));
  return ok;
}

}  // namespace graphics::virgl_patch
