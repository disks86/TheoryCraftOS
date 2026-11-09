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
};

// The uniform state of the compatibility profile (GLSL 1.20, 7.5): each becomes tcos_<name> in both stages, declared with the structure type it needs.
struct StateUniform {
  const char* name;          // as the shader writes it
  const char* declaration;   // after `uniform `, with the name as tcos_...
  int8_t structure;          // index in kStateStructs, or -1
};
constexpr const char* kStateStructs[] = {
    "struct tcos_DepthRangeParameters { float near; float far; float diff; };\n",
    "struct tcos_PointParameters { float size; float sizeMin; float sizeMax; float fadeThresholdSize; float distanceConstantAttenuation; float distanceLinearAttenuation; float distanceQuadraticAttenuation; };\n",
    "struct tcos_MaterialParameters { vec4 emission; vec4 ambient; vec4 diffuse; vec4 specular; float shininess; };\n",
    "struct tcos_LightSourceParameters { vec4 ambient; vec4 diffuse; vec4 specular; vec4 position; vec4 halfVector; vec3 spotDirection; float spotExponent; float spotCutoff; float spotCosCutoff; float constantAttenuation; float linearAttenuation; float quadraticAttenuation; };\n",
    "struct tcos_LightModelParameters { vec4 ambient; };\n",
    "struct tcos_LightModelProducts { vec4 sceneColor; };\n",
    "struct tcos_LightProducts { vec4 ambient; vec4 diffuse; vec4 specular; };\n",
    "struct tcos_FogParameters { vec4 color; float density; float start; float end; float scale; };\n",
};
constexpr StateUniform kState[] = {
    {"gl_DepthRange", "tcos_DepthRangeParameters tcos_DepthRange", 0},
    {"gl_Point", "tcos_PointParameters tcos_Point", 1},
    {"gl_FrontMaterial", "tcos_MaterialParameters tcos_FrontMaterial", 2},
    {"gl_BackMaterial", "tcos_MaterialParameters tcos_BackMaterial", 2},
    {"gl_LightSource", "tcos_LightSourceParameters tcos_LightSource[8]", 3},
    {"gl_LightModel", "tcos_LightModelParameters tcos_LightModel", 4},
    {"gl_FrontLightModelProduct", "tcos_LightModelProducts tcos_FrontLightModelProduct", 5},
    {"gl_BackLightModelProduct", "tcos_LightModelProducts tcos_BackLightModelProduct", 5},
    {"gl_FrontLightProduct", "tcos_LightProducts tcos_FrontLightProduct[8]", 6},
    {"gl_BackLightProduct", "tcos_LightProducts tcos_BackLightProduct[8]", 6},
    {"gl_Fog", "tcos_FogParameters tcos_Fog", 7},
    {"gl_ClipPlane", "vec4 tcos_ClipPlane[8]", -1},
    {"gl_TextureEnvColor", "vec4 tcos_TextureEnvColor[8]", -1},
    {"gl_EyePlaneS", "vec4 tcos_EyePlaneS[8]", -1}, {"gl_EyePlaneT", "vec4 tcos_EyePlaneT[8]", -1}, {"gl_EyePlaneR", "vec4 tcos_EyePlaneR[8]", -1}, {"gl_EyePlaneQ", "vec4 tcos_EyePlaneQ[8]", -1},
    {"gl_ObjectPlaneS", "vec4 tcos_ObjectPlaneS[8]", -1}, {"gl_ObjectPlaneT", "vec4 tcos_ObjectPlaneT[8]", -1}, {"gl_ObjectPlaneR", "vec4 tcos_ObjectPlaneR[8]", -1}, {"gl_ObjectPlaneQ", "vec4 tcos_ObjectPlaneQ[8]", -1},
    {"gl_ModelViewMatrixTranspose", "mat4 tcos_ModelViewMatrixTranspose", -1}, {"gl_ModelViewMatrixInverseTranspose", "mat4 tcos_ModelViewMatrixInverseTranspose", -1},
    {"gl_ProjectionMatrixInverse", "mat4 tcos_ProjectionMatrixInverse", -1}, {"gl_ProjectionMatrixTranspose", "mat4 tcos_ProjectionMatrixTranspose", -1},
    {"gl_ProjectionMatrixInverseTranspose", "mat4 tcos_ProjectionMatrixInverseTranspose", -1},
    {"gl_ModelViewProjectionMatrixInverse", "mat4 tcos_ModelViewProjectionMatrixInverse", -1}, {"gl_ModelViewProjectionMatrixTranspose", "mat4 tcos_ModelViewProjectionMatrixTranspose", -1},
    {"gl_ModelViewProjectionMatrixInverseTranspose", "mat4 tcos_ModelViewProjectionMatrixInverseTranspose", -1},
    {"gl_TextureMatrixInverse", "mat4 tcos_TextureMatrixInverse[8]", -1}, {"gl_TextureMatrixTranspose", "mat4 tcos_TextureMatrixTranspose[8]", -1},
    {"gl_TextureMatrixInverseTranspose", "mat4 tcos_TextureMatrixInverseTranspose[8]", -1},
};

// The first #version directive, past white space and comments (GLSL lets nothing else come before it): its number, the end of its line, and its line number.
int32_t parse_version(StringView text, uint64_t* line_end, uint32_t* line, bool* es) {
  *line_end = 0;
  *line = 0;
  *es = false;
  uint64_t i = 0;
  uint32_t lines = 1;
  for (;;) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) { if (text[i] == '\n') ++lines; ++i; }
    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') { while (i < text.size() && text[i] != '\n') ++i; continue; }
    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) { if (text[i] == '\n') ++lines; ++i; }
      i = i + 2 < text.size() ? i + 2 : text.size();
      continue;
    }
    break;
  }
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
  *line_end = end < text.size() ? end + 1 : end;
  *line = lines;
  return v;
}

}  // namespace

bool translate_desktop_glsl(StringView source, bool vertex, String* out, uint32_t* uses) {
  *uses = 0;
  uint64_t body = 0;
  uint32_t version_line = 0;
  bool es = false;
  int32_t version = parse_version(source, &body, &version_line, &es);
  if (es || version == 100 || version == 300 || version == 310 || version == 320) return false;      // GLSL ES: as it is
  if (version < 0) { version = 110; body = 0; }
  const bool modern = version >= 130;
  // Which built-ins are named (whole identifiers only), so only those are declared.
  bool used[sizeof(kRenames) / sizeof(kRenames[0])] = {};
  bool used_state[sizeof(kState) / sizeof(kState[0])] = {};
  const StringView text = source.substr(body, source.size() - body);
  for (uint64_t i = 0; i < text.size();) {
    if (!ident_char(text[i]) || (i > 0 && ident_char(text[i - 1]))) { ++i; continue; }
    uint64_t j = i;
    while (j < text.size() && ident_char(text[j])) ++j;
    const StringView word = text.substr(i, j - i);
    for (uint64_t r = 0; r < sizeof(kRenames) / sizeof(kRenames[0]); ++r) if (word.equals(kRenames[r].from)) used[r] = true;
    for (uint64_t r = 0; r < sizeof(kState) / sizeof(kState[0]); ++r) if (word.equals(kState[r].name)) used_state[r] = true;
    i = j;
  }
  out->clear();
  // The shader's own #version line stays first; the declarations follow it.
  out->append(source.substr(0, body));
  if (body && (source[body - 1] != '\n')) out->append("\n");
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
    }
    if (name.equals("gl_ModelViewMatrix")) decl("uniform", "mat4", "tcos_ModelViewMatrix", "");
    else if (name.equals("gl_ProjectionMatrix")) decl("uniform", "mat4", "tcos_ProjectionMatrix", "");
    else if (name.equals("gl_ModelViewProjectionMatrix") || (vertex && name.equals("ftransform") && !used[21])) { if (!name.equals("ftransform") || true) decl("uniform", "mat4", "tcos_ModelViewProjectionMatrix", ""); }
    else if (name.equals("gl_NormalMatrix")) decl("uniform", "mat3", "tcos_NormalMatrix", "");
    else if (name.equals("gl_TextureMatrix")) decl("uniform", "mat4", "tcos_TextureMatrix", "[8]");
    else if (name.equals("gl_ModelViewMatrixInverse")) decl("uniform", "mat4", "tcos_ModelViewMatrixInverse", "");
    else if (name.equals("gl_NormalScale")) decl("uniform", "float", "tcos_NormalScale", "");
  }
  // The uniform state, each structure type once.
  bool struct_done[sizeof(kStateStructs) / sizeof(kStateStructs[0])] = {};
  for (uint64_t r = 0; r < sizeof(kState) / sizeof(kState[0]); ++r) {
    if (!used_state[r]) continue;
    *uses |= kUseState;
    const int8_t st = kState[r].structure;
    if (st >= 0 && !struct_done[st]) { out->append(kStateStructs[st]); struct_done[st] = true; }
    out->append("uniform ");
    out->append(kState[r].declaration);
    out->append(";\n");
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
  {      // (the text after the declarations keeps its line numbers)
    char line[24] = "#line ";
    uint32_t n = version_line + 1, k = 6;
    char digits[12];
    uint32_t d = 0;
    do { digits[d++] = static_cast<char>('0' + n % 10); n /= 10; } while (n && d < sizeof(digits));
    while (d) line[k++] = digits[--d];
    line[k++] = '\n';
    out->append(StringView(line, k));
  }
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
    char state_name[64];
    for (const StateUniform& st : kState) {
      if (!word.equals(st.name) || word.size() + 3 > sizeof(state_name)) continue;
      // gl_X -> tcos_X
      const char* prefix = "tcos_";
      uint64_t k = 0;
      for (; prefix[k]; ++k) state_name[k] = prefix[k];
      for (uint64_t c = 3; c < word.size(); ++c) state_name[k++] = word[c];
      state_name[k] = 0;
      replacement = state_name;
    }
    if (!replacement) for (const Rename& r : kRenames) {
      if (!word.equals(r.from)) continue;
      replacement = vertex ? r.vertex_to : r.fragment_to;
      break;
    }
    out->append(replacement ? StringView(replacement) : word);
    i = j;
  }
  return true;
}

}  // namespace opengl
