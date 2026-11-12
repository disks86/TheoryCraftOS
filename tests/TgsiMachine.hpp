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

// A TGSI interpreter for host tests: runs the text a translator wrote (src/Kernel/Dxgkrnl/D3d9Tgsi.cpp, src/WebGl/TgsiTranslator.cpp) on registers of floats, so a test
// can compare what the device would compute with the CPU's own interpreter. Slow (it reads the text each run) and only for tests.
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"

namespace tgsi_test {

// (the instructions that read their registers as integers: a source's - and || are integer ones for them)
inline bool tgsi_integer_op(const std::string& op) {
  static const char* const kOps[] = {"UCMP", "USEQ", "USNE", "ISLT", "ISGE", "USLT", "USGE", "UADD", "INEG", "UMUL", "UMAD", "IMIN", "IMAX", "UMIN", "UMAX", "UDIV", "UMOD", "AND", "OR", "XOR", "NOT",
                                     "I2F", "U2F", "SHL", "ISHR", "USHR"};
  for (const char* o : kOps)
    if (op == o) return true;
  return false;
}
inline bool tgsi_if(const std::string& op) { return op == "IF" || op == "UIF"; }

class TgsiMachine {
 public:
  static constexpr int kRegs = 64, kVertices = 32;
  float temp[1024][4] = {}, in[kRegs][4] = {}, out[kRegs][4] = {}, cst[4100][4] = {}, imm[256][4] = {}, sv[8][4] = {};   // (sv: VERTEXID, INSTANCEID ...)
  // The geometry and tessellation stages: inputs per vertex (IN[v][r]), a control shader's outputs per control point (OUT[v][r]), and a geometry shader's
  // emitted vertices (all its OUT registers at each EMIT; `cut` says a primitive ended before the vertex).
  float in2[kVertices][kRegs][4] = {}, out2[kVertices][kRegs][4] = {};
  struct Emitted { float out[kRegs][4]; bool cut; };
  std::vector<Emitted> emitted;
  bool pending_cut = false;
  bool stop_at_barrier = false;   // a BARRIER stops run() / resume() (else it does nothing)
  bool at_barrier = false;        // ... and they stopped at one
  int addr = 0;
  bool killed = false;
  const graphics::ITextureSet* textures = nullptr;

  // Runs `text`; false when it does not parse or uses something the machine has not got.
  bool run(const std::string& text) {
    std::vector<std::string>& body = body_;
    body.clear();
    pc_ = 0;
    loops_.clear();
    steps_ = 0;
    at_barrier = false;
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find('\n', at);
      if (end == std::string::npos) end = text.size();
      std::string l = text.substr(at, end - at);
      at = end + 1;
      size_t b = l.find_first_not_of(' ');
      if (b == std::string::npos) continue;
      l = l.substr(b);
      if (l.rfind("IMM[", 0) == 0) {
        const unsigned i = static_cast<unsigned>(atoi(l.c_str() + 4));
        const char* p = strchr(l.c_str(), '{');
        if (!p || i >= 256) return false;
        ++p;
        const bool bits = l.find("UINT32") != std::string::npos;   // (an integer literal, in decimal: the register holds its bits)
        for (int k = 0; k < 4; ++k) {
          char* e = nullptr;
          if (bits) imm[i][k] = __builtin_bit_cast(float, static_cast<uint32_t>(strtoul(p, &e, 10)));
          else imm[i][k] = strtof(p, &e);
          if (e == p) return false;
          p = e;
          while (*p == ',' || *p == ' ') ++p;
        }
      } else if (isdigit(static_cast<unsigned char>(l[0]))) {
        body.push_back(l.substr(l.find(':') + 2));
      }
    }
    // Structure.
    std::vector<size_t> else_of(body.size(), 0), end_of(body.size(), 0), stack;
    for (size_t i = 0; i < body.size(); ++i) {
      const std::string op = body[i].substr(0, body[i].find(' '));
      if (tgsi_if(op)) stack.push_back(i);
      else if (op == "ELSE") {
        if (stack.empty()) return false;
        else_of[stack.back()] = i;
        stack.back() = i;   // (the ELSE now stands for the open construct)
        end_of[i] = 0;
      } else if (op == "ENDIF") {
        if (stack.empty()) return false;
        end_of[stack.back()] = i;
        stack.pop_back();
      }
    }
    if (!stack.empty()) return false;
    return resume();
  }
  // Goes on after a BARRIER (run() and resume() return there with at_barrier set: a control shader's invocations are run side by side to each one).
  bool resume() {
    at_barrier = false;
    std::vector<std::string>& body = body_;
    std::vector<size_t>& loops = loops_;
    uint32_t& steps = steps_;
    size_t& pc = pc_;
    while (pc < body.size()) {
      const std::string& l = body[pc];
      const size_t sp = l.find(' ');
      const std::string op = l.substr(0, sp);
      std::vector<std::string> args;
      if (sp != std::string::npos) {
        std::string rest = l.substr(sp + 1);
        size_t s = 0;
        while (s <= rest.size()) {
          size_t c = rest.find(',', s);
          if (c == std::string::npos) c = rest.size();
          std::string a = rest.substr(s, c - s);
          const size_t f = a.find_first_not_of(' ');
          args.push_back(f == std::string::npos ? "" : a.substr(f));
          s = c + 1;
        }
      }
      ++pc;
      if (op == "END") break;
      if (op == "BARRIER") {
        if (!stop_at_barrier) continue;
        at_barrier = true;
        return true;
      }
      if (op == "ELSE") {   // the true branch ended: to the ENDIF
        size_t e = pc;
        int depth = 0;
        for (; e < body.size(); ++e) {
          const std::string o = body[e].substr(0, body[e].find(' '));
          if (tgsi_if(o)) ++depth;
          if (o == "ENDIF") {
            if (!depth) break;
            --depth;
          }
        }
        pc = e + 1;
        continue;
      }
      if (op == "ENDIF") continue;
      if (++steps > 100000) return false;
      if (op == "BGNLOOP") {
        loops.push_back(pc);
        continue;
      }
      if (op == "ENDLOOP") {
        if (loops.empty()) return false;
        pc = loops.back();
        continue;
      }
      if (op == "BRK" || op == "CONT") {
        size_t e = pc;
        int depth = 0;
        for (; e < body.size(); ++e) {
          const std::string o = body[e].substr(0, body[e].find(' '));
          if (o == "BGNLOOP") ++depth;
          if (o == "ENDLOOP") {
            if (!depth) break;
            --depth;
          }
        }
        if (e >= body.size() || loops.empty()) return false;
        if (op == "BRK") {
          loops.pop_back();
          pc = e + 1;
        } else {
          pc = e;   // (the ENDLOOP takes it back to the top)
        }
        continue;
      }
      if (tgsi_if(op)) {
        float c[4];
        if (!fetch(args.at(0), c, op == "UIF")) return false;
        if (op == "UIF" ? __builtin_bit_cast(uint32_t, c[0]) == 0 : c[0] == 0) {   // to after the ELSE or the ENDIF
          size_t e = pc;
          int depth = 0;
          for (; e < body.size(); ++e) {
            const std::string o = body[e].substr(0, body[e].find(' '));
            if (tgsi_if(o)) ++depth;
            else if ((o == "ELSE" || o == "ENDIF") && !depth) break;
            else if (o == "ENDIF") --depth;
          }
          pc = e + 1;
        }
        continue;
      }
      if (!execute(op, args)) { if (getenv("TGSI_TRACE")) printf("  TGSI machine cannot run: %s\n", l.c_str()); return false; }
    }
    return true;
  }

 private:
  std::vector<std::string> body_;
  std::vector<size_t> loops_;
  size_t pc_ = 0;
  uint32_t steps_ = 0;
  struct Ref {
    bool neg = false, abs = false;
    std::string file;
    int index = 0;
    int dim = -1;   // FILE[dim][index]
    bool relative = false;
    std::string swz;
  };
  static bool parse(const std::string& in_text, Ref* r) {
    std::string s = in_text;
    if (s.empty()) return false;
    if (s[0] == '-') r->neg = true, s = s.substr(1);
    if (s[0] == '|') {
      r->abs = true;
      s = s.substr(1);
      if (s.empty() || s.back() != '|') return false;
      s.pop_back();
    }
    const size_t lb = s.find('[');
    if (lb == std::string::npos) return false;
    r->file = s.substr(0, lb);
    int depth = 0;
    size_t rb = lb;
    for (size_t i = lb; i < s.size(); ++i) {
      if (s[i] == '[') ++depth;
      if (s[i] == ']' && --depth == 0) {
        rb = i;
        break;
      }
    }
    if (rb == lb) return false;
    std::string inner = s.substr(lb + 1, rb - lb - 1);
    if (rb + 1 < s.size() && s[rb + 1] == '[') {      // two-dimensional: [vertex][register]
      const size_t rb2 = s.find(']', rb + 1);
      if (rb2 == std::string::npos) return false;
      r->dim = atoi(inner.c_str());
      inner = s.substr(rb + 2, rb2 - rb - 2);
      rb = rb2;
    }
    if (inner.rfind("ADDR[0].x+", 0) == 0) r->relative = true, r->index = atoi(inner.c_str() + 10);
    else r->index = atoi(inner.c_str());
    if (rb + 1 < s.size()) {
      if (s[rb + 1] != '.') return false;
      r->swz = s.substr(rb + 2);
    }
    return true;
  }
  float* reg(const Ref& r) {
    int index = r.index + (r.relative ? addr : 0);
    static float zero[4];
    zero[0] = zero[1] = zero[2] = zero[3] = 0;
    if (r.dim >= 0) {
      if (r.dim >= kVertices || index < 0 || index >= kRegs) return nullptr;
      if (r.file == "IN") return in2[r.dim][index];
      if (r.file == "OUT") return out2[r.dim][index];
      return nullptr;
    }
    if (r.file == "TEMP") return index >= 0 && index < 1024 ? temp[index] : nullptr;
    if (r.file == "IN") return index >= 0 && index < kRegs ? in[index] : nullptr;
    if (r.file == "SV") return index >= 0 && index < 8 ? sv[index] : nullptr;
    if (r.file == "OUT") return index >= 0 && index < kRegs ? out[index] : nullptr;
    if (r.file == "IMM") return index >= 0 && index < 256 ? imm[index] : nullptr;
    if (r.file == "CONST") return index >= 0 && index < 4100 ? cst[index] : zero;
    return nullptr;
  }
  bool fetch(const std::string& text, float v[4], bool integer = false) {
    Ref r;
    if (!parse(text, &r)) return false;
    const float* p = reg(r);
    if (!p) return false;
    for (int i = 0; i < 4; ++i) {
      int ch = i;
      if (!r.swz.empty()) {
        if (r.swz.size() != 4) return false;
        ch = static_cast<int>(strchr("xyzw", r.swz[i]) - "xyzw");
      }
      v[i] = p[ch];
      if (integer) {
        int32_t x = __builtin_bit_cast(int32_t, v[i]);
        if (r.abs && x < 0) x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
        if (r.neg) x = static_cast<int32_t>(0u - static_cast<uint32_t>(x));
        v[i] = __builtin_bit_cast(float, x);
        continue;
      }
      if (r.abs) v[i] = fabsf(v[i]);
      if (r.neg) v[i] = -v[i];
    }
    return true;
  }
  bool store(const std::string& text, const float v[4]) {
    Ref r;
    if (!parse(text, &r)) return false;
    float* p = reg(r);
    if (!p) return false;
    const std::string mask = r.swz.empty() ? "xyzw" : r.swz;
    for (char c : mask) {
      const char* k = strchr("xyzw", c);
      if (!k) return false;
      p[k - "xyzw"] = v[k - "xyzw"];
    }
    return true;
  }
  bool execute(const std::string& op_in, const std::vector<std::string>& args) {
    // OP_SAT: the result clamped to [0, 1]
    const bool sat = op_in.size() > 4 && op_in.compare(op_in.size() - 4, 4, "_SAT") == 0;
    const std::string op = sat ? op_in.substr(0, op_in.size() - 4) : op_in;
    float a[4] = {}, b[4] = {}, c[4] = {}, r[4] = {};
    auto need = [&](size_t n) { return args.size() >= n; };
    if (op == "ARL" || op == "ARR" || op == "UARL") {
      if (!need(2) || !fetch(args[1], a)) return false;
      addr = op == "UARL" ? static_cast<int>(__builtin_bit_cast(uint32_t, a[0])) : static_cast<int>(floorf(op == "ARR" ? a[0] + 0.5f : a[0]));
      return true;
    }
    if (op == "KILL") {
      killed = true;
      return true;
    }
    if (op == "EMIT") {
      Emitted e;
      memcpy(e.out, out, sizeof out);
      e.cut = pending_cut;
      pending_cut = false;
      emitted.push_back(e);
      return true;
    }
    if (op == "ENDPRIM") {
      pending_cut = true;
      return true;
    }
    if (op == "MEMBAR") return true;
    if (op == "KILL_IF") {
      if (!need(1) || !fetch(args[0], a)) return false;
      for (float x : a) killed = killed || x < 0;
      return true;
    }
    const bool integer = tgsi_integer_op(op), is_tex = op == "TEX" || op == "TXP" || op == "TXF" || op == "TXL" || op == "TXQ";
    const bool data_integer = integer && op != "UCMP";   // (UCMP's condition is an integer, what it selects is data)
    if (!need(2) || !fetch(args[1], a, integer)) return false;
    if (args.size() > 2 && !is_tex && !fetch(args[2], b, data_integer)) return false;
    if (args.size() > 3 && !is_tex && !fetch(args[3], c, data_integer)) return false;
    const bool scalar = op == "RCP" || op == "RSQ" || op == "EX2" || op == "LG2" || op == "COS" || op == "SIN";
    for (int i = 0; i < 4; ++i) {
      if (op == "MOV") r[i] = a[i];
      else if (op == "ADD") r[i] = a[i] + b[i];
      else if (op == "SUB") r[i] = a[i] - b[i];
      else if (op == "MUL") r[i] = a[i] * b[i];
      else if (op == "MAD") r[i] = a[i] * b[i] + c[i];
      else if (op == "DIV") r[i] = a[i] / b[i];
      else if (op == "LRP") r[i] = a[i] * b[i] + (1.0f - a[i]) * c[i];
      else if (op == "XPD") r[i] = i == 0 ? a[1] * b[2] - a[2] * b[1] : i == 1 ? a[2] * b[0] - a[0] * b[2] : i == 2 ? a[0] * b[1] - a[1] * b[0] : 1.0f;
      else if (op == "MIN") r[i] = a[i] < b[i] ? a[i] : b[i];
      else if (op == "MAX") r[i] = a[i] > b[i] ? a[i] : b[i];
      else if (op == "SLT") r[i] = a[i] < b[i] ? 1.0f : 0.0f;
      else if (op == "SGE") r[i] = a[i] >= b[i] ? 1.0f : 0.0f;
      else if (op == "SEQ") r[i] = a[i] == b[i] ? 1.0f : 0.0f;
      else if (op == "FSEQ") r[i] = __builtin_bit_cast(float, a[i] == b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "FSNE") r[i] = __builtin_bit_cast(float, a[i] != b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "FSLT") r[i] = __builtin_bit_cast(float, a[i] < b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "FSGE") r[i] = __builtin_bit_cast(float, a[i] >= b[i] ? 0xFFFFFFFFu : 0u);
      else if (op == "SNE") r[i] = a[i] != b[i] ? 1.0f : 0.0f;
      else if (op == "CMP") r[i] = a[i] < 0 ? b[i] : c[i];
      else if (op == "FRC") r[i] = a[i] - floorf(a[i]);
      else if (op == "ABS") r[i] = fabsf(a[i]);
      else if (op == "SSG") r[i] = a[i] > 0 ? 1.0f : a[i] < 0 ? -1.0f : 0.0f;
      else if (op == "DP2") r[i] = a[0] * b[0] + a[1] * b[1];
      else if (op == "DP3") r[i] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      else if (op == "DP4") r[i] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
      else if (op == "RCP") r[i] = 1.0f / a[0];
      else if (op == "RSQ") r[i] = 1.0f / sqrtf(fabsf(a[0]));
      else if (op == "EX2") r[i] = exp2f(a[0]);
      else if (op == "LG2") r[i] = log2f(a[0]);
      else if (op == "COS") r[i] = cosf(a[0]);
      else if (op == "SIN") r[i] = sinf(a[0]);
      else if (op == "POW") r[i] = powf(a[0], b[0]);
      else if (op == "SQRT") r[i] = sqrtf(a[0]);
      else if (op == "CEIL") r[i] = ceilf(a[i]);
      else if (op == "FLR") r[i] = floorf(a[i]);
      else if (op == "TRUNC") r[i] = truncf(a[i]);
      else if (op == "ROUND") r[i] = nearbyintf(a[i]);
      else if (integer) {
        const uint32_t ua = __builtin_bit_cast(uint32_t, a[i]), ub = __builtin_bit_cast(uint32_t, b[i]), uc = __builtin_bit_cast(uint32_t, c[i]);
        const int32_t ia = static_cast<int32_t>(ua), ib = static_cast<int32_t>(ub);
        uint32_t u = 0;
        bool as_float = false;
        float f = 0;
        if (op == "UCMP") u = ua != 0 ? ub : uc;
        else if (op == "USEQ") u = ua == ub ? ~0u : 0u;
        else if (op == "USNE") u = ua != ub ? ~0u : 0u;
        else if (op == "ISLT") u = ia < ib ? ~0u : 0u;
        else if (op == "ISGE") u = ia >= ib ? ~0u : 0u;
        else if (op == "USLT") u = ua < ub ? ~0u : 0u;
        else if (op == "USGE") u = ua >= ub ? ~0u : 0u;
        else if (op == "UADD") u = ua + ub;
        else if (op == "INEG") u = 0u - ua;
        else if (op == "UMUL") u = ua * ub;
        else if (op == "UMAD") u = ua * ub + uc;
        else if (op == "IMIN") u = static_cast<uint32_t>(ia < ib ? ia : ib);
        else if (op == "IMAX") u = static_cast<uint32_t>(ia > ib ? ia : ib);
        else if (op == "UMIN") u = ua < ub ? ua : ub;
        else if (op == "UMAX") u = ua > ub ? ua : ub;
        else if (op == "UDIV") u = ub ? ua / ub : ~0u;
        else if (op == "UMOD") u = ub ? ua % ub : ~0u;
        else if (op == "AND") u = ua & ub;
        else if (op == "OR") u = ua | ub;
        else if (op == "XOR") u = ua ^ ub;
        else if (op == "NOT") u = ~ua;
        else if (op == "SHL") u = ua << (ub & 31);
        else if (op == "ISHR") u = static_cast<uint32_t>(ia >> (ub & 31));
        else if (op == "USHR") u = ua >> (ub & 31);
        else if (op == "I2F") as_float = true, f = static_cast<float>(ia);
        else if (op == "U2F") as_float = true, f = static_cast<float>(ua);
        else return false;
        r[i] = as_float ? f : __builtin_bit_cast(float, u);
      } else if (op == "F2I") {
        r[i] = __builtin_bit_cast(float, static_cast<uint32_t>(static_cast<int32_t>(a[i])));
      } else if (op == "F2U") {
        r[i] = __builtin_bit_cast(float, a[i] <= 0 ? 0u : static_cast<uint32_t>(a[i]));
      } else if (!is_tex) return false;
    }
    (void)scalar;
    if (op == "TXF" || op == "TXL" || op == "TXQ") {
      if (!need(4) || !textures) return false;
      const unsigned unit = static_cast<unsigned>(atoi(args[2].c_str() + 5));
      graphics::Vec4 t = {0, 0, 0, 0};
      if (op == "TXF") t = textures->fetch(unit, __builtin_bit_cast(int32_t, a[0]), __builtin_bit_cast(int32_t, a[1]), __builtin_bit_cast(uint32_t, a[3]));
      else if (op == "TXL") t = textures->sample_lod(unit, a[0], a[1], a[3]);
      else {
        uint32_t w = 0, h = 0, levels = 0;
        if (!textures->size(unit, __builtin_bit_cast(uint32_t, a[0]), &w, &h, &levels)) return false;
        t = {__builtin_bit_cast(float, w), __builtin_bit_cast(float, h), 0.0f, __builtin_bit_cast(float, levels)};
      }
      r[0] = t.x, r[1] = t.y, r[2] = t.z, r[3] = t.w;
    }
    if (op == "TEX" || op == "TXP") {
      if (!need(4) || !textures) return false;
      const unsigned unit = static_cast<unsigned>(atoi(args[2].c_str() + 5));
      float u = a[0], v = a[1];
      if (op == "TXP") {
        const float inv = a[3] == 0 ? 0.0f : 1.0f / a[3];
        u *= inv, v *= inv;
      }
      const graphics::Vec4 t = textures->sample(unit, u, v);
      r[0] = t.x, r[1] = t.y, r[2] = t.z, r[3] = t.w;
    }
    if (sat)
      for (float& x : r) x = x < 0 ? 0.0f : (x > 1 ? 1.0f : x);
    return store(args[0], r);
  }
};


}  // namespace tgsi_test
