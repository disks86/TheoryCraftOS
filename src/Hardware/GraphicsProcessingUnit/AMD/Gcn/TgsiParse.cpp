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


#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/TgsiParse.hpp"

namespace gcn {

namespace {

struct Scanner {
  StringView s;
  uint64_t pos = 0;
  bool done() const { return pos >= s.size(); }
  char peek() const { return pos < s.size() ? s[pos] : '\0'; }
  void skip_space() {
    while (!done() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) ++pos;
  }
  bool accept(char c) {
    skip_space();
    if (peek() == c) {
      ++pos;
      return true;
    }
    return false;
  }
  bool word(StringView* out) {        // letters, digits and underscores
    skip_space();
    const uint64_t begin = pos;
    while (!done() && ((peek() >= 'A' && peek() <= 'Z') || (peek() >= 'a' && peek() <= 'z') || (peek() >= '0' && peek() <= '9') || peek() == '_')) ++pos;
    if (pos == begin) return false;
    *out = s.substr(begin, pos - begin);
    return true;
  }
  bool uint_value(uint32_t* out) {
    skip_space();
    if (!(peek() >= '0' && peek() <= '9')) return false;
    uint64_t v = 0;
    while (peek() >= '0' && peek() <= '9') {
      v = v * 10 + static_cast<uint64_t>(peek() - '0');
      if (v > 0xFFFFFFFFu) return false;
      ++pos;
    }
    *out = static_cast<uint32_t>(v);
    return true;
  }
  // a decimal float: [-+]digits[.digits][e[-+]digits]
  bool float_value(float* out) {
    skip_space();
    bool neg = false;
    if (peek() == '-' || peek() == '+') {
      neg = peek() == '-';
      ++pos;
    }
    if (!((peek() >= '0' && peek() <= '9') || peek() == '.')) return false;
    double v = 0;
    while (peek() >= '0' && peek() <= '9') {
      v = v * 10 + static_cast<double>(peek() - '0');
      ++pos;
    }
    if (peek() == '.') {
      ++pos;
      double scale = 0.1;
      while (peek() >= '0' && peek() <= '9') {
        v += scale * static_cast<double>(peek() - '0');
        scale *= 0.1;
        ++pos;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos;
      bool eneg = false;
      if (peek() == '-' || peek() == '+') {
        eneg = peek() == '-';
        ++pos;
      }
      uint32_t e = 0;
      if (!uint_value(&e) || e > 60) return false;
      for (uint32_t i = 0; i < e; ++i) v = eneg ? v / 10.0 : v * 10.0;
    }
    if (v > 3.4e38) return false;
    *out = static_cast<float>(neg ? -v : v);
    return true;
  }
};

bool file_of(StringView w, TgsiFile* f) {
  if (w.equals(StringView("TEMP"))) *f = TgsiFile::Temp;
  else if (w.equals(StringView("IN"))) *f = TgsiFile::In;
  else if (w.equals(StringView("OUT"))) *f = TgsiFile::Out;
  else if (w.equals(StringView("CONST"))) *f = TgsiFile::Const;
  else if (w.equals(StringView("IMM"))) *f = TgsiFile::Imm;
  else return false;
  return true;
}

struct OpName {
  const char* name;
  TgsiOp op;
  uint8_t sources;
};
constexpr OpName kOps[] = {
    {"MOV", TgsiOp::Mov, 1}, {"ADD", TgsiOp::Add, 2}, {"SUB", TgsiOp::Sub, 2}, {"MUL", TgsiOp::Mul, 2}, {"MAD", TgsiOp::Mad, 3}, {"DIV", TgsiOp::Div, 2}, {"LRP", TgsiOp::Lrp, 3},
    {"MAX", TgsiOp::Max, 2}, {"MIN", TgsiOp::Min, 2}, {"ABS", TgsiOp::Abs, 1}, {"FRC", TgsiOp::Frc, 1}, {"FLR", TgsiOp::Flr, 1}, {"CEIL", TgsiOp::Ceil, 1}, {"TRUNC", TgsiOp::Trunc, 1},
    {"ROUND", TgsiOp::Round, 1}, {"SSG", TgsiOp::Ssg, 1}, {"RCP", TgsiOp::Rcp, 1}, {"RSQ", TgsiOp::Rsq, 1}, {"SQRT", TgsiOp::Sqrt, 1}, {"EX2", TgsiOp::Ex2, 1}, {"LG2", TgsiOp::Lg2, 1},
    {"POW", TgsiOp::Pow, 2}, {"SIN", TgsiOp::Sin, 1}, {"COS", TgsiOp::Cos, 1}, {"DP2", TgsiOp::Dp2, 2}, {"DP3", TgsiOp::Dp3, 2}, {"DP4", TgsiOp::Dp4, 2}, {"XPD", TgsiOp::Xpd, 2},
    {"CMP", TgsiOp::Cmp, 3}, {"SEQ", TgsiOp::Seq, 2}, {"SNE", TgsiOp::Sne, 2}, {"SGE", TgsiOp::Sge, 2}, {"SLT", TgsiOp::Slt, 2}, {"SGT", TgsiOp::Sgt, 2}, {"SLE", TgsiOp::Sle, 2},
    {"TEX", TgsiOp::Tex, 1}, {"TXB", TgsiOp::Txb, 1}, {"TXL", TgsiOp::Txl, 1}, {"KILL", TgsiOp::Kill, 0}, {"KILL_IF", TgsiOp::KillIf, 1}, {"IF", TgsiOp::If, 1}, {"ELSE", TgsiOp::Else, 0},
    {"ENDIF", TgsiOp::EndIf, 0}, {"END", TgsiOp::End, 0},
};

bool swizzle_letter(char c, uint8_t* out) {
  switch (c) {
    case 'x': *out = 0; return true;
    case 'y': *out = 1; return true;
    case 'z': *out = 2; return true;
    case 'w': *out = 3; return true;
    default: return false;
  }
}

// file[index](.xyzw) with the optional '-' and '|' of a source
bool parse_register(Scanner& sc, TgsiFile* file, uint32_t* index, bool* has_swizzle, uint8_t swz[4], uint8_t* mask, bool as_dst, bool* neg, bool* abs) {
  *neg = false;
  *abs = false;
  if (!as_dst) {
    if (sc.accept('-')) *neg = true;
    if (sc.accept('|')) *abs = true;
  }
  StringView w;
  if (!sc.word(&w) || !file_of(w, file)) return false;
  if (!sc.accept('[') || !sc.uint_value(index) || !sc.accept(']')) return false;
  *has_swizzle = false;
  if (sc.peek() == '.') {
    ++sc.pos;
    StringView letters;
    if (!sc.word(&letters)) return false;
    if (as_dst) {
      uint8_t m = 0;
      for (uint64_t i = 0; i < letters.size(); ++i) {
        uint8_t c;
        if (!swizzle_letter(letters[i], &c)) return false;
        m = static_cast<uint8_t>(m | (1u << c));
      }
      *mask = m;
    } else {
      if (letters.size() == 0 || letters.size() > 4) return false;
      for (uint32_t k = 0; k < 4; ++k) {
        uint8_t c;
        if (!swizzle_letter(letters[k < letters.size() ? k : letters.size() - 1], &c)) return false;       // (a short swizzle repeats its last letter)
        swz[k] = c;
      }
      *has_swizzle = true;
    }
  }
  if (!as_dst && *abs && !sc.accept('|')) return false;
  return true;
}

}  // namespace

namespace {

bool target_of(StringView w, uint8_t* out) {
  if (w.equals(StringView("2D"))) *out = TgsiShader::kTarget2D;
  else if (w.equals(StringView("3D"))) *out = TgsiShader::kTarget3D;
  else if (w.equals(StringView("CUBE"))) *out = TgsiShader::kTargetCube;
  else if (w.equals(StringView("2D_ARRAY"))) *out = TgsiShader::kTarget2DArray;
  else return false;
  return true;
}

}  // namespace

bool parse_tgsi(allocators::IAllocator& allocator, StringView text, TgsiShader* out, const char** error, uint32_t* line_out) {
  (void)allocator;
  const char* err = nullptr;
  uint32_t line_no = 0;
  bool have_stage = false, have_end = false;
  uint64_t at = 0;
  while (at < text.size() && !err) {
    uint64_t end = text.find('\n', at);
    if (end == StringView::npos) end = text.size();
    StringView line = text.substr(at, end - at);
    at = end + 1;
    ++line_no;
    const uint64_t comment = line.find(';');
    if (comment != StringView::npos) line = line.first(comment);
    line = line.trimmed();
    if (line.empty()) continue;
    if (have_end) { err = "text after END"; break; }
    Scanner sc{line, 0};
    if (line.equals(StringView("VERT")) || line.equals(StringView("FRAG"))) {
      out->fragment = line[0] == 'F';
      have_stage = true;
      continue;
    }
    if (!have_stage) { err = "the first line must be VERT or FRAG"; break; }
    if (line.starts_with(StringView("PROPERTY"))) continue;
    if (line.starts_with(StringView("DCL "))) {
      sc.pos = 4;
      StringView kind;
      uint32_t first = 0, last = 0;
      if (!sc.word(&kind) || !sc.accept('[') || !sc.uint_value(&first)) { err = "malformed DCL"; break; }
      last = first;
      if (sc.accept('.')) {
        if (!sc.accept('.') || !sc.uint_value(&last)) { err = "malformed DCL range"; break; }
      }
      if (!sc.accept(']') || last < first) { err = "malformed DCL"; break; }
      const uint32_t count = last + 1;
      TgsiSemantic semantic = TgsiSemantic::None;
      uint32_t semantic_index = 0;
      bool flat = false;
      if (kind.equals(StringView("SVIEW"))) {                // ", 2D, FLOAT": the target, then the type (float is all there is)
        uint8_t target = TgsiShader::kTarget2D;
        StringView t;
        if (sc.accept(',') && sc.word(&t)) {
          if (!target_of(t, &target)) { err = "unsupported texture target"; break; }
        }
        for (uint32_t k = first; k <= last && k < TgsiShader::kMaxSamplers; ++k) out->sampler_target[k] = target;
        continue;
      }
      // optional ", SEMANTIC[n]" and ", INTERP"
      while (sc.accept(',')) {
        StringView w;
        if (!sc.word(&w)) break;
        if (w.equals(StringView("POSITION"))) semantic = TgsiSemantic::Position;
        else if (w.equals(StringView("COLOR"))) semantic = TgsiSemantic::Color;
        else if (w.equals(StringView("GENERIC"))) semantic = TgsiSemantic::Generic;
        else if (w.equals(StringView("FOG"))) semantic = TgsiSemantic::Fog;
        else if (w.equals(StringView("PSIZE"))) semantic = TgsiSemantic::PointSize;
        else if (w.equals(StringView("FACE"))) semantic = TgsiSemantic::Face;
        else if (w.equals(StringView("INSTANCEID"))) semantic = TgsiSemantic::Instance;
        else if (w.equals(StringView("VERTEXID"))) semantic = TgsiSemantic::VertexId;
        else if (w.equals(StringView("CONSTANT"))) flat = true;
        else if (w.equals(StringView("PERSPECTIVE")) || w.equals(StringView("LINEAR")) || w.equals(StringView("COLOR"))) {}
        else { err = "unknown DCL attribute"; break; }
        if (sc.accept('[')) {
          if (!sc.uint_value(&semantic_index) || !sc.accept(']')) { err = "malformed semantic index"; break; }
        }
      }
      if (err) break;
      if (kind.equals(StringView("IN"))) {
        if (last >= TgsiShader::kMaxInputs) { err = "too many inputs"; break; }
        for (uint32_t i = first; i <= last; ++i) {
          out->input_semantic[i] = semantic;
          out->input_semantic_index[i] = static_cast<uint8_t>(semantic_index);
          out->input_flat[i] = flat;
        }
        out->inputs = out->inputs > count ? out->inputs : count;
      } else if (kind.equals(StringView("OUT"))) {
        if (last >= TgsiShader::kMaxOutputs) { err = "too many outputs"; break; }
        for (uint32_t i = first; i <= last; ++i) {
          out->output_semantic[i] = semantic;
          out->output_semantic_index[i] = static_cast<uint8_t>(semantic_index);
        }
        out->outputs = out->outputs > count ? out->outputs : count;
      } else if (kind.equals(StringView("CONST"))) {
        if (last >= TgsiShader::kMaxConsts) { err = "too many constants"; break; }
        out->consts = out->consts > count ? out->consts : count;
      } else if (kind.equals(StringView("TEMP"))) {
        if (last >= TgsiShader::kMaxTemps) { err = "too many temporaries"; break; }
        out->temps = out->temps > count ? out->temps : count;
      } else if (kind.equals(StringView("SAMP"))) {
        if (last >= TgsiShader::kMaxSamplers) { err = "too many samplers"; break; }
        out->samplers = out->samplers > count ? out->samplers : count;
      } else {
        err = "unknown DCL kind";
        break;
      }
      continue;
    }
    if (line.starts_with(StringView("IMM["))) {
      sc.pos = 4;
      uint32_t index = 0;
      StringView type;
      if (!sc.uint_value(&index) || !sc.accept(']') || !sc.word(&type) || !type.equals(StringView("FLT32")) || !sc.accept('{')) { err = "malformed IMM"; break; }
      if (index != out->immediates.count() || index >= TgsiShader::kMaxImmediates) { err = "immediates must be declared in order"; break; }
      TgsiImmediate imm;
      for (uint32_t k = 0; k < 4; ++k) {
        if (k && !sc.accept(',')) { err = "an immediate has four values"; break; }
        if (!sc.float_value(&imm.value[k])) { err = "malformed immediate value"; break; }
      }
      if (err) break;
      if (!sc.accept('}') || !out->immediates.push_back(imm)) { err = "malformed IMM"; break; }
      continue;
    }
    // an instruction, optionally numbered "12:"
    sc.skip_space();
    {
      const uint64_t save = sc.pos;
      uint32_t number;
      if (sc.uint_value(&number) && sc.accept(':')) {}
      else sc.pos = save;
    }
    StringView name;
    if (!sc.word(&name)) { err = "expected an instruction"; break; }
    bool saturate = false;
    if (name.size() > 4 && name.last(4).equals(StringView("_SAT"))) {
      saturate = true;
      name = name.without_suffix(4);
    }
    const OpName* found = nullptr;
    for (const OpName& o : kOps)
      if (name.equals(StringView(o.name))) found = &o;
    if (!found) { err = "unknown instruction"; break; }
    TgsiInst inst;
    inst.op = found->op;
    inst.line = line_no;
    const bool has_dst = found->op != TgsiOp::Kill && found->op != TgsiOp::KillIf && found->op != TgsiOp::If && found->op != TgsiOp::Else && found->op != TgsiOp::EndIf && found->op != TgsiOp::End;
    if (has_dst) {
      TgsiFile f;
      uint32_t idx;
      bool has_swz, neg, abs;
      uint8_t swz[4], mask = 0xF;
      if (!parse_register(sc, &f, &idx, &has_swz, swz, &mask, true, &neg, &abs) || (f != TgsiFile::Temp && f != TgsiFile::Out)) { err = "bad destination"; break; }
      if (idx > 0xFFFF) { err = "register index too big"; break; }
      inst.dst.file = f;
      inst.dst.index = static_cast<uint16_t>(idx);
      inst.dst.mask = mask;
      inst.dst.saturate = saturate;
    }
    for (uint32_t s = 0; s < found->sources && !err; ++s) {
      if ((has_dst || s > 0) && !sc.accept(',')) { err = "missing source"; break; }
      TgsiFile f;
      uint32_t idx;
      bool has_swz, neg, abs;
      uint8_t swz[4] = {0, 1, 2, 3}, mask = 0;
      if (!parse_register(sc, &f, &idx, &has_swz, swz, &mask, false, &neg, &abs) || f == TgsiFile::Out || idx > 0xFFFF) { err = "bad source"; break; }
      TgsiSrc& src = inst.src[s];
      src.file = f;
      src.index = static_cast<uint16_t>(idx);
      for (uint32_t k = 0; k < 4; ++k) src.swizzle[k] = swz[k];
      src.negate = neg;
      src.absolute = abs;
      ++inst.src_count;
    }
    if (err) break;
    if (found->op == TgsiOp::Tex || found->op == TgsiOp::Txb || found->op == TgsiOp::Txl) {
      StringView w;
      uint32_t sampler = 0;
      if (!sc.accept(',') || !sc.word(&w) || !w.equals(StringView("SAMP")) || !sc.accept('[') || !sc.uint_value(&sampler) || !sc.accept(']') || sampler >= TgsiShader::kMaxSamplers) { err = "TEX needs a sampler"; break; }
      inst.sampler = static_cast<uint8_t>(sampler);
      if (sc.accept(',')) {
        uint8_t target = 0;
        if (!sc.word(&w) || !target_of(w, &target)) { err = "unsupported texture target"; break; }
        if (target != out->sampler_target[sampler]) { err = "the texture target is not the one declared"; break; }
      }
    }
    sc.skip_space();
    if (!sc.done()) { err = "text after the instruction"; break; }
    if (!out->instructions.push_back(inst)) { err = "out of memory"; break; }
    if (found->op == TgsiOp::End) have_end = true;
  }
  if (!err && !have_end) err = "no END";
  if (!err && !have_stage) err = "no VERT or FRAG";
  if (error) *error = err;
  if (line_out) *line_out = line_no;
  return err == nullptr;
}

}  // namespace gcn
