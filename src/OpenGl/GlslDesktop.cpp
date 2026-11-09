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

#include "OpenGl/GlslDesktop.hpp"

namespace opengl {

namespace {

bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

struct Rename {
  const char* from;
  const char* vertex_to;      // in the vertex stage (null: not renamed there)
  const char* fragment_to;
  uint32_t use;
};

// What each built-in becomes, and the declaration it needs (by stage, with %s for "attribute"/"in", "varying"/"out"/"in").
constexpr Rename kRenames[] = {
    {"gl_Vertex", "tcos_Vertex", nullptr, 0},
    {"gl_Normal", "tcos_Normal", nullptr, 0},
    {"gl_Color", "tcos_Color", "tcos_FrontColor", 0},
    {"gl_SecondaryColor", "tcos_SecondaryColor", "tcos_FrontSecondaryColor", 0},
    {"gl_FogCoord", "tcos_FogCoord", nullptr, 0},
    {"gl_MultiTexCoord0", "tcos_MultiTexCoord0", nullptr, 0}, {"gl_MultiTexCoord1", "tcos_MultiTexCoord1", nullptr, 0}, {"gl_MultiTexCoord2", "tcos_MultiTexCoord2", nullptr, 0},
    {"gl_MultiTexCoord3", "tcos_MultiTexCoord3", nullptr, 0}, {"gl_MultiTexCoord4", "tcos_MultiTexCoord4", nullptr, 0}, {"gl_MultiTexCoord5", "tcos_MultiTexCoord5", nullptr, 0},
    {"gl_MultiTexCoord6", "tcos_MultiTexCoord6", nullptr, 0}, {"gl_MultiTexCoord7", "tcos_MultiTexCoord7", nullptr, 0},
    {"gl_FrontColor", "tcos_FrontColor", nullptr, 0},
    {"gl_BackColor", "tcos_BackColor", nullptr, 0},
    {"gl_FrontSecondaryColor", "tcos_FrontSecondaryColor", nullptr, 0},
    {"gl_BackSecondaryColor", "tcos_BackSecondaryColor", nullptr, 0},
    {"gl_TexCoord", "tcos_TexCoord", "tcos_TexCoord", 0},
    {"gl_FogFragCoord", "tcos_FogFragCoord", "tcos_FogFragCoord", 0},
    {"gl_ModelViewMatrix", "tcos_ModelViewMatrix", "tcos_ModelViewMatrix", kUseModelView},
    {"gl_ProjectionMatrix", "tcos_ProjectionMatrix", "tcos_ProjectionMatrix", kUseProjection},
    {"gl_ModelViewProjectionMatrix", "tcos_ModelViewProjectionMatrix", "tcos_ModelViewProjectionMatrix", kUseMvp},
    {"gl_NormalMatrix", "tcos_NormalMatrix", "tcos_NormalMatrix", kUseNormalMatrix},
    {"gl_TextureMatrix", "tcos_TextureMatrix", "tcos_TextureMatrix", kUseTextureMatrix},
    {"gl_ModelViewMatrixInverse", "tcos_ModelViewMatrixInverse", "tcos_ModelViewMatrixInverse", kUseModelViewInverse},
    {"gl_NormalScale", "tcos_NormalScale", "tcos_NormalScale", kUseNormalScale},
    {"ftransform", "tcos_ftransform", nullptr, kUseMvp},
    {"gl_FragColor", nullptr, "tcos_FragColor", 0},
    {"gl_FragData", nullptr, "tcos_FragData", 0},
    {"texture2D", "texture", "texture", 0}, {"texture3D", "texture", "texture", 0}, {"textureCube", "texture", "texture", 0}, {"shadow2D", "texture", "texture", 0},
    {"texture2DProj", "textureProj", "textureProj", 0}, {"texture2DLod", "textureLod", "textureLod", 0}, {"texture2DRect", "texture", "texture", 0},
    {"sampler2DRect", "sampler2D", "sampler2D", 0},
};

int32_t parse_version(StringView text, uint64_t* line_end, bool* es) {
  // The first #version directive (before any other code, as GLSL requires; comments are not looked into).
  *line_end = 0;
  *es = false;
  uint64_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
  const StringView directive("#version");
  if (text.size() - i < directive.size() || !text.substr(i, directive.size()).equals(directive)) return -1;
  i += directive.size();
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  int32_t v = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9' && v < 10000) v = v * 10 + (text[i++] - '0');
  uint64_t end = i;
  while (end < text.size() && text[end] != '\n') ++end;
  const StringView rest = text.substr(i, end - i);
  for (uint64_t k = 0; k + 1 < rest.size(); ++k)
    if (rest[k] == 'e' && rest[k + 1] == 's' && (k == 0 || !ident_char(rest[k - 1])) && (k + 2 >= rest.size() || !ident_char(rest[k + 2]))) *es = true;
  *line_end = end;
  return v;
}

}  // namespace

bool translate_desktop_glsl(StringView source, bool vertex, String* out, uint32_t* uses) {
  *uses = 0;
  uint64_t body = 0;
  bool es = false;
  int32_t version = parse_version(source, &body, &es);
  if (es || version == 100) return false;      // already GLSL ES
  if (version < 0) { version = 110; body = 0; }
  const bool modern = version >= 130;
  // Which built-ins are named (whole identifiers only), so only those are declared.
  bool used[sizeof(kRenames) / sizeof(kRenames[0])] = {};
  const StringView text = source.substr(body, source.size() - body);
  for (uint64_t i = 0; i < text.size();) {
    if (!ident_char(text[i]) || (i > 0 && ident_char(text[i - 1]))) { ++i; continue; }
    uint64_t j = i;
    while (j < text.size() && ident_char(text[j])) ++j;
    const StringView word = text.substr(i, j - i);
    for (uint64_t r = 0; r < sizeof(kRenames) / sizeof(kRenames[0]); ++r) if (word.equals(kRenames[r].from)) used[r] = true;
    i = j;
  }
  out->clear();
  out->append(modern ? "#version 300 es\n" : "#version 100\n");
  out->append("precision highp float;\nprecision highp int;\n");
  if (modern) out->append("precision highp sampler3D;\nprecision highp samplerCube;\nprecision highp sampler2DShadow;\nprecision highp sampler2DArray;\n");
  const char* attribute = modern ? "in" : "attribute";
  const char* varying_out = modern ? "out" : "varying";
  const char* varying_in = modern ? "in" : "varying";
  const auto decl = [&](const char* qualifier, const char* type, const char* name, const char* array) {
    out->append(qualifier);
    out->append(" ");
    out->append(type);
    out->append(" ");
    out->append(name);
    out->append(array);
    out->append(";\n");
  };
  for (uint64_t r = 0; r < sizeof(kRenames) / sizeof(kRenames[0]); ++r) {
    if (!used[r]) continue;
    const StringView name(kRenames[r].from);
    *uses |= kRenames[r].use;
    if (vertex) {
      if (name.equals("gl_Vertex")) decl(attribute, "vec4", "tcos_Vertex", "");
      else if (name.equals("gl_Normal")) decl(attribute, "vec3", "tcos_Normal", "");
      else if (name.equals("gl_Color")) decl(attribute, "vec4", "tcos_Color", "");
      else if (name.equals("gl_SecondaryColor")) decl(attribute, "vec4", "tcos_SecondaryColor", "");
      else if (name.equals("gl_FogCoord")) decl(attribute, "float", "tcos_FogCoord", "");
      else if (name.starts_with("gl_MultiTexCoord")) decl(attribute, "vec4", kRenames[r].vertex_to, "");
      else if (name.equals("gl_FrontColor") || name.equals("gl_BackColor") || name.equals("gl_FrontSecondaryColor") || name.equals("gl_BackSecondaryColor")) decl(varying_out, "vec4", kRenames[r].vertex_to, "");
      else if (name.equals("gl_TexCoord")) decl(varying_out, "vec4", "tcos_TexCoord", "[8]");
      else if (name.equals("gl_FogFragCoord")) decl(varying_out, "float", "tcos_FogFragCoord", "");
    } else {
      if (name.equals("gl_Color")) decl(varying_in, "vec4", "tcos_FrontColor", "");
      else if (name.equals("gl_SecondaryColor")) decl(varying_in, "vec4", "tcos_FrontSecondaryColor", "");
      else if (name.equals("gl_TexCoord")) decl(varying_in, "vec4", "tcos_TexCoord", "[8]");
      else if (name.equals("gl_FogFragCoord")) decl(varying_in, "float", "tcos_FogFragCoord", "");
      else if (name.equals("gl_FragColor") && modern) decl("out", "vec4", "tcos_FragColor", "");
      else if (name.equals("gl_FragData") && modern) decl("out", "vec4", "tcos_FragData", "[8]");
    }
    if (name.equals("gl_ModelViewMatrix")) decl("uniform", "mat4", "tcos_ModelViewMatrix", "");
    else if (name.equals("gl_ProjectionMatrix")) decl("uniform", "mat4", "tcos_ProjectionMatrix", "");
    else if (name.equals("gl_ModelViewProjectionMatrix") || (vertex && name.equals("ftransform") && !used[21])) { if (!name.equals("ftransform") || true) decl("uniform", "mat4", "tcos_ModelViewProjectionMatrix", ""); }
    else if (name.equals("gl_NormalMatrix")) decl("uniform", "mat3", "tcos_NormalMatrix", "");
    else if (name.equals("gl_TextureMatrix")) decl("uniform", "mat4", "tcos_TextureMatrix", "[8]");
    else if (name.equals("gl_ModelViewMatrixInverse")) decl("uniform", "mat4", "tcos_ModelViewMatrixInverse", "");
    else if (name.equals("gl_NormalScale")) decl("uniform", "float", "tcos_NormalScale", "");
  }
  // ftransform() needs the vertex too.
  bool ftransform = false;
  for (uint64_t r = 0; r < sizeof(kRenames) / sizeof(kRenames[0]); ++r) if (used[r] && StringView(kRenames[r].from).equals("ftransform")) ftransform = true;
  bool have_vertex = false;
  for (uint64_t r = 0; r < sizeof(kRenames) / sizeof(kRenames[0]); ++r) if (used[r] && StringView(kRenames[r].from).equals("gl_Vertex")) have_vertex = true;
  if (vertex && ftransform) {
    if (!have_vertex) decl(attribute, "vec4", "tcos_Vertex", "");
    out->append("vec4 tcos_ftransform() { return tcos_ModelViewProjectionMatrix * tcos_Vertex; }\n");
  }
  // The rest of the text, built-ins renamed; `#version` gone, and in a 300 es translation the desktop keywords attribute/varying become in/out.
  out->append("#line 2\n");
  for (uint64_t i = 0; i < text.size();) {
    if (!ident_char(text[i]) || (i > 0 && ident_char(text[i - 1]))) {
      const char c[2] = {text[i], 0};
      out->append(StringView(c, 1));
      ++i;
      continue;
    }
    uint64_t j = i;
    while (j < text.size() && ident_char(text[j])) ++j;
    const StringView word = text.substr(i, j - i);
    const char* replacement = nullptr;
    for (const Rename& r : kRenames) {
      if (!word.equals(r.from)) continue;
      const bool is_texture_fn = StringView(r.from).starts_with("texture") || StringView(r.from).starts_with("shadow") || StringView(r.from).starts_with("sampler2DRect");
      if (is_texture_fn && !modern) {
        replacement = StringView(r.from).equals("sampler2DRect") ? "sampler2D" : StringView(r.from).equals("texture2DRect") ? "texture2D" : nullptr;
        break;
      }
      replacement = vertex ? r.vertex_to : r.fragment_to;
      if (!modern && (StringView(r.from).equals("gl_FragColor") || StringView(r.from).equals("gl_FragData"))) replacement = nullptr;
      break;
    }
    if (modern && !replacement && (word.equals("attribute") || word.equals("varying"))) replacement = word.equals("attribute") ? "in" : (vertex ? "out" : "in");
    if (!replacement && (word.equals("core") || word.equals("compatibility"))) replacement = "";
    out->append(replacement ? StringView(replacement) : word);
    i = j;
  }
  return true;
}

}  // namespace opengl
