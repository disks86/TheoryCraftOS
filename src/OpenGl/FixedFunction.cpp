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

// The fixed-function pipeline as shaders. The state that changes what must be computed (which lights, which texture environment ...) is a key; each key gets a program written in
// GLSL here, compiled once and kept. State that only changes numbers goes in uniforms. So the fixed-function pipeline, user shaders and the 3D hardware path all draw the same way.
#include "OpenGl/FixedFunction.hpp"
#include "OpenGl/GlApi.hpp"
#include "OpenGl/generated/GlEnums.inc"

namespace opengl {

namespace {

constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

bool is_identity(const Mat4& m) {
  for (int i = 0; i < 16; ++i) if (m.m[i] != kIdentity[i]) return false;
  return true;
}

// Text builder: a failed allocation leaves a shader that does not compile, which is reported as GL_OUT_OF_MEMORY-free failure of the draw.
struct Text {
  String s;
  explicit Text(allocators::IAllocator& a) : s(a) {}
  Text& operator<<(const char* t) { s.append(t); return *this; }
  Text& operator<<(uint32_t v) {
    char buf[12];
    int n = 0;
    do { buf[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    char rev[12];
    for (int i = 0; i < n; ++i) rev[i] = buf[n - 1 - i];
    s.append(StringView(rev, static_cast<uint64_t>(n)));
    return *this;
  }
};

const char* const kCoord[4] = {"s", "t", "r", "q"};

uint32_t base_class(uint32_t base) {
  switch (base) {
    case GL_ALPHA: return kBaseAlpha;
    case GL_LUMINANCE: return kBaseLuminance;
    case GL_LUMINANCE_ALPHA: return kBaseLuminanceAlpha;
    case GL_INTENSITY: return kBaseIntensity;
    case GL_RED: case GL_RG: case GL_RGB: case GL_DEPTH_COMPONENT: case GL_DEPTH_STENCIL: return kBaseRgb;
    default: return kBaseRgba;
  }
}

uint32_t combine_code(uint32_t mode) {
  switch (mode) {
    case GL_REPLACE: return 0;
    case GL_MODULATE: return 1;
    case GL_ADD: return 2;
    case GL_ADD_SIGNED: return 3;
    case GL_INTERPOLATE: return 4;
    case GL_SUBTRACT: return 5;
    case GL_DOT3_RGB: return 6;
    case GL_DOT3_RGBA: return 7;
    default: return 1;
  }
}

uint32_t source_code(uint32_t s) {
  if (s == GL_TEXTURE) return 0;
  if (s == GL_PREVIOUS) return 1;
  if (s == GL_CONSTANT) return 2;
  if (s == GL_PRIMARY_COLOR) return 3;
  if (s >= GL_TEXTURE0 && s < GL_TEXTURE0 + kMaxTexUnits) return 4 + (s - GL_TEXTURE0);
  return 1;
}

void color_source(Text& t, uint32_t unit, uint32_t code) {
  switch (code) {
    case 0: t << "tex" << unit; break;
    case 1: t << "prev"; break;
    case 2: t << "u_ec" << unit; break;
    case 3: t << "prim"; break;
    default: t << "tex" << (code - 4); break;
  }
}

// One argument of a combiner: colour.
void rgb_arg(Text& t, uint32_t unit, uint32_t source, uint32_t operand) {
  const bool alpha = operand == 2 || operand == 3, invert = operand == 1 || operand == 3;
  t << "(";
  if (invert) t << "vec3(1.0)-";
  if (alpha) { t << "vec3("; color_source(t, unit, source); t << ".a)"; }
  else { color_source(t, unit, source); t << ".rgb"; }
  t << ")";
}

void alpha_arg(Text& t, uint32_t unit, uint32_t source, uint32_t operand) {
  t << "(";
  if (operand == 1) t << "1.0-";
  color_source(t, unit, source);
  t << ".a)";
}

void light_code(Text& t, const FixedKey& k, uint32_t i, const char* n, const char* face, const char* c, const char* s) {
  t << "  { vec3 L; float att=1.0;\n";
  if ((k.light_positional >> i) & 1) {
    t << "    vec3 d=u_lp" << i << ".xyz-ec.xyz; float dist=length(d); L=d/dist;\n";
    t << "    att=1.0/(u_la" << i << ".x+u_la" << i << ".y*dist+u_la" << i << ".z*dist*dist);\n";
    if ((k.light_spot >> i) & 1) t << "    float sd=dot(-L,u_ld" << i << "); att*= sd<u_lc" << i << " ? 0.0 : pow(sd,u_le" << i << ");\n";
  } else {
    t << "    L=normalize(u_lp" << i << ".xyz);\n";
  }
  t << "    float nl=max(dot(" << n << ",L),0.0);\n";
  t << "    " << c << "+=att*(ma" << face << ".rgb*u_lA" << i << ".rgb+nl*md" << face << ".rgb*u_lD" << i << ".rgb);\n";
  t << "    if(nl>0.0){ vec3 h=normalize(L+eye); float nh=max(dot(" << n << ",h),0.0); " << s << "+=att*(nh>0.0?pow(nh,u_shin" << face << "):0.0)*ms" << face << ".rgb*u_lS" << i << ".rgb; }\n";
  t << "  }\n";
}

void write_vertex_shader(Text& t, const FixedKey& k) {
  t << "#version 300 es\nprecision highp float;\nprecision highp int;\n";
  t << "in vec4 ff_Vertex;\nin vec3 ff_Normal;\nin vec4 ff_Color;\nin vec4 ff_Color2;\nin float ff_Fog;\n";
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) if (k.tex_needs_coord & (1u << i)) t << "in vec4 ff_Tex" << i << ";\nuniform mat4 u_tm" << i << ";\n";
  t << "uniform mat4 u_mvp;\nuniform mat4 u_mv;\nuniform mat3 u_nm;\nuniform float u_nscale;\nuniform float u_ps;\nuniform vec3 u_pa;\nuniform vec2 u_pr;\n";
  // Varyings: the colours first, because a flat shade model is a range of them.
  t << "out vec4 v_c;\nout vec3 v_s;\n";
  if (k.two_side) t << "out vec4 v_bc;\nout vec3 v_bs;\n";
  t << "out float v_fog;\n";
  for (uint32_t i = 0; i < kMaxClipPlanes; ++i) if (k.clip_mask & (1u << i)) t << "out float v_cl" << i << ";\nuniform vec4 u_cp" << i << ";\n";
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) if (k.tex_needs_coord & (1u << i)) t << "out vec4 v_t" << i << ";\n";
  if (k.lighting) {
    t << "uniform vec4 u_ma0,u_md0,u_ms0,u_me0;\nuniform float u_shin0;\nuniform vec4 u_ma1,u_md1,u_ms1,u_me1;\nuniform float u_shin1;\nuniform vec4 u_model;\n";
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (!((k.lights >> i) & 1)) continue;
      t << "uniform vec4 u_lA" << i << ",u_lD" << i << ",u_lS" << i << ",u_lp" << i << ";\nuniform vec3 u_la" << i << ",u_ld" << i << ";\nuniform float u_lc" << i << ",u_le" << i << ";\n";
    }
  }
  for (uint32_t i = 0; i < kMaxTexUnits; ++i)
    for (uint32_t c = 0; c < 4; ++c)
      if ((k.texgen[i] >> (c * 3)) & 7) t << "uniform vec4 u_gp" << i << kCoord[c] << ";\n";
  t << "void main(){\n  gl_Position=u_mvp*ff_Vertex;\n";
  if (k.needs_eye) t << "  vec4 ec=u_mv*ff_Vertex;\n";
  if (k.needs_normal) {
    t << "  vec3 n=u_nm*ff_Normal;\n";
    if (k.normalize) t << "  n=normalize(n);\n";
    else if (k.rescale) t << "  n*=u_nscale;\n";
  }
  if (k.lighting) {
    t << "  vec3 eye=" << (k.local_viewer ? "normalize(-ec.xyz)" : "vec3(0.0,0.0,1.0)") << ";\n";
    const auto material = [&](const char* f, const char* front_or_back) {
      const bool f_side = front_or_back[0] == 'f';
      const bool on = k.color_material && (k.color_material_face == 2 || (k.color_material_face == 0) == f_side);
      t << "  vec4 ma" << f << "=" ; if (on && (k.color_material_mode == 0 || k.color_material_mode == 3)) t << "ff_Color"; else t << "u_ma" << f; t << ";\n";
      t << "  vec4 md" << f << "=" ; if (on && (k.color_material_mode == 1 || k.color_material_mode == 3)) t << "ff_Color"; else t << "u_md" << f; t << ";\n";
      t << "  vec4 ms" << f << "=" ; if (on && k.color_material_mode == 2) t << "ff_Color"; else t << "u_ms" << f; t << ";\n";
      t << "  vec4 me" << f << "=" ; if (on && k.color_material_mode == 4) t << "ff_Color"; else t << "u_me" << f; t << ";\n";
    };
    material("0", "front");
    t << "  vec3 c0=me0.rgb+ma0.rgb*u_model.rgb; vec3 s0=vec3(0.0);\n";
    for (uint32_t i = 0; i < kMaxLights; ++i) if ((k.lights >> i) & 1) light_code(t, k, i, "n", "0", "c0", "s0");
    if (k.separate_specular) t << "  v_c=vec4(clamp(c0,0.0,1.0),clamp(md0.a,0.0,1.0)); v_s=clamp(s0,0.0,1.0);\n";
    else t << "  v_c=vec4(clamp(c0+s0,0.0,1.0),clamp(md0.a,0.0,1.0)); v_s=vec3(0.0);\n";
    if (k.two_side) {
      material("1", "back");
      t << "  vec3 nb=-n; vec3 c1=me1.rgb+ma1.rgb*u_model.rgb; vec3 s1=vec3(0.0);\n";
      for (uint32_t i = 0; i < kMaxLights; ++i) if ((k.lights >> i) & 1) light_code(t, k, i, "nb", "1", "c1", "s1");
      if (k.separate_specular) t << "  v_bc=vec4(clamp(c1,0.0,1.0),clamp(md1.a,0.0,1.0)); v_bs=clamp(s1,0.0,1.0);\n";
      else t << "  v_bc=vec4(clamp(c1+s1,0.0,1.0),clamp(md1.a,0.0,1.0)); v_bs=vec3(0.0);\n";
    }
  } else {
    t << "  v_c=clamp(ff_Color,0.0,1.0); v_s=clamp(ff_Color2.rgb,0.0,1.0);\n";
    if (k.two_side) t << "  v_bc=v_c; v_bs=v_s;\n";
  }
  if (k.fog) t << "  v_fog=" << (k.fog_from_coord ? "abs(ff_Fog)" : "abs(ec.z)") << ";\n"; else t << "  v_fog=0.0;\n";
  for (uint32_t i = 0; i < kMaxClipPlanes; ++i) if (k.clip_mask & (1u << i)) t << "  v_cl" << i << "=dot(u_cp" << i << ",ec);\n";
  if (k.point_attenuated) t << "  { float dp=length(ec.xyz); float ps=u_ps*sqrt(1.0/(u_pa.x+u_pa.y*dp+u_pa.z*dp*dp)); gl_PointSize=clamp(ps,u_pr.x,u_pr.y); }\n";
  else t << "  gl_PointSize=clamp(u_ps,u_pr.x,u_pr.y);\n";
  if (k.needs_reflection) t << "  vec3 eu=normalize(ec.xyz); vec3 rf=eu-2.0*n*dot(n,eu); float rm=2.0*sqrt(rf.x*rf.x+rf.y*rf.y+(rf.z+1.0)*(rf.z+1.0));\n";
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
    if (!(k.tex_needs_coord & (1u << i))) continue;
    if (!k.texgen[i]) {
      if ((k.tex_matrix >> i) & 1) t << "  v_t" << i << "=u_tm" << i << "*ff_Tex" << i << ";\n";
      else t << "  v_t" << i << "=ff_Tex" << i << ";\n";
      continue;
    }
    t << "  { vec4 g=ff_Tex" << i << ";\n";
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t mode = (k.texgen[i] >> (c * 3)) & 7;
      const char* name = kCoord[c];
      const char* lane = c == 0 ? "x" : c == 1 ? "y" : c == 2 ? "z" : "w";
      switch (mode) {
        case kGenObject: t << "    g." << lane << "=dot(u_gp" << i << name << ",ff_Vertex);\n"; break;
        case kGenEye: t << "    g." << lane << "=dot(u_gp" << i << name << ",ec);\n"; break;
        case kGenSphere: t << "    g." << lane << "=rf." << lane << "/rm+0.5;\n"; break;
        case kGenNormal: t << "    g." << lane << "=n." << lane << ";\n"; break;
        case kGenReflection: t << "    g." << lane << "=rf." << lane << ";\n"; break;
        default: break;
      }
    }
    t << "    v_t" << i << "=";
    if ((k.tex_matrix >> i) & 1) t << "u_tm" << i << "*";
    t << "g; }\n";
  }
  t << "}\n";
}

void write_sample(Text& t, const FixedKey& k, uint32_t i) {
  const uint32_t dim = k.tex_dim[i];
  t << "  vec4 tex" << i << "=";
  const char* const bias = k.tex_bias & (1u << i) ? ",u_tb" : "";
  if (k.tex_shadow & (1u << i)) {
    t << "vec4(vec3(texture(u_s" << i << ",vec3(v_t" << i << ".st/v_t" << i << ".q,v_t" << i << ".p/v_t" << i << ".q))),1.0);\n";
    return;
  }
  switch (dim) {
    case kDimFixed3D: t << "texture(u_s" << i << ",v_t" << i << ".stp/v_t" << i << ".q"; break;
    case kDimFixedCube: t << "texture(u_s" << i << ",v_t" << i << ".stp"; break;
    case kDimFixedRect: t << "texture(u_s" << i << ",v_t" << i << ".st/v_t" << i << ".q*u_rs" << i; break;
    case kDimFixed1D: t << "texture(u_s" << i << ",vec2(v_t" << i << ".s/v_t" << i << ".q,0.5)"; break;
    default: t << "texture(u_s" << i << ",v_t" << i << ".st/v_t" << i << ".q"; break;
  }
  if (*bias) t << bias << i;
  t << ");\n";
}

void write_env(Text& t, const FixedKey& k, uint32_t i) {
  const uint32_t mode = k.tex_mode[i], base = k.tex_base[i];
  const StringView s = "";
  (void)s;
  t << "  // unit " << i << "\n";
  if (mode == kEnvCombine) {
    const FixedKey::Combine& c = k.combine[i];
    t << "  { vec3 a0=";
    rgb_arg(t, i, c.src_rgb[0], c.op_rgb[0]);
    t << "; vec3 a1=";
    rgb_arg(t, i, c.src_rgb[1], c.op_rgb[1]);
    t << "; vec3 a2=";
    rgb_arg(t, i, c.src_rgb[2], c.op_rgb[2]);
    t << ";\n    float b0=";
    alpha_arg(t, i, c.src_a[0], c.op_a[0]);
    t << "; float b1=";
    alpha_arg(t, i, c.src_a[1], c.op_a[1]);
    t << "; float b2=";
    alpha_arg(t, i, c.src_a[2], c.op_a[2]);
    t << ";\n    vec3 cr; float ca;\n";
    switch (c.mode_rgb) {
      case 0: t << "    cr=a0;\n"; break;
      case 1: t << "    cr=a0*a1;\n"; break;
      case 2: t << "    cr=a0+a1;\n"; break;
      case 3: t << "    cr=a0+a1-vec3(0.5);\n"; break;
      case 4: t << "    cr=a0*a2+a1*(vec3(1.0)-a2);\n"; break;
      case 5: t << "    cr=a0-a1;\n"; break;
      default: t << "    cr=vec3(4.0*dot(a0-vec3(0.5),a1-vec3(0.5)));\n"; break;
    }
    switch (c.mode_a) {
      case 0: t << "    ca=b0;\n"; break;
      case 1: t << "    ca=b0*b1;\n"; break;
      case 2: t << "    ca=b0+b1;\n"; break;
      case 3: t << "    ca=b0+b1-0.5;\n"; break;
      case 4: t << "    ca=b0*b2+b1*(1.0-b2);\n"; break;
      default: t << "    ca=b0-b1;\n"; break;
    }
    if (c.mode_rgb == 7) t << "    ca=cr.r;\n";
    t << "    prev=vec4(clamp(cr*u_es" << i << ".x,0.0,1.0),clamp(ca*u_es" << i << ".y,0.0,1.0)); }\n";
    return;
  }
  const char* out_rgb = "prev.rgb";
  const char* out_a = "prev.a";
  (void)out_rgb; (void)out_a;
  t << "  { vec3 cs=tex" << i << ".rgb; float as=tex" << i << ".a; vec3 cc=u_ec" << i << ".rgb; float ac=u_ec" << i << ".a;\n    ";
  switch (mode) {
    case kEnvReplace:
      if (base == kBaseAlpha) t << "prev.a=as;";
      else if (base == kBaseLuminance || base == kBaseRgb) t << "prev.rgb=cs;";
      else t << "prev=vec4(cs,as);";
      break;
    case kEnvDecal:
      if (base == kBaseRgb) t << "prev.rgb=cs;";
      else if (base == kBaseRgba) t << "prev.rgb=mix(prev.rgb,cs,as);";
      break;
    case kEnvBlend:
      if (base == kBaseAlpha) t << "prev.a*=as;";
      else if (base == kBaseLuminance || base == kBaseRgb) t << "prev.rgb=mix(prev.rgb,cc,cs);";
      else if (base == kBaseIntensity) t << "prev.rgb=mix(prev.rgb,cc,cs); prev.a=mix(prev.a,ac,as);";
      else t << "prev.rgb=mix(prev.rgb,cc,cs); prev.a*=as;";
      break;
    case kEnvAdd:
      if (base == kBaseAlpha) t << "prev.a*=as;";
      else if (base == kBaseLuminance || base == kBaseRgb) t << "prev.rgb=clamp(prev.rgb+cs,0.0,1.0);";
      else if (base == kBaseIntensity) t << "prev=clamp(prev+vec4(cs,as),0.0,1.0);";
      else t << "prev.rgb=clamp(prev.rgb+cs,0.0,1.0); prev.a*=as;";
      break;
    default:      // modulate
      if (base == kBaseAlpha) t << "prev.a*=as;";
      else if (base == kBaseLuminance || base == kBaseRgb) t << "prev.rgb*=cs;";
      else t << "prev*=vec4(cs,as);";
      break;
  }
  t << " }\n";
}

void write_fragment_shader(Text& t, const FixedKey& k) {
  t << "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler3D;\nprecision highp samplerCube;\nprecision highp sampler2DShadow;\n";
  t << "in vec4 v_c;\nin vec3 v_s;\n";
  if (k.two_side) t << "in vec4 v_bc;\nin vec3 v_bs;\n";
  t << "in float v_fog;\n";
  for (uint32_t i = 0; i < kMaxClipPlanes; ++i) if (k.clip_mask & (1u << i)) t << "in float v_cl" << i << ";\n";
  t << "out vec4 ff_out;\n";
  bool any_tex = false;
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
    if (k.tex_needs_coord & (1u << i)) t << "in vec4 v_t" << i << ";\n";
    if (!k.tex_dim[i]) continue;
    any_tex = true;
    const uint32_t dim = k.tex_dim[i];
    t << "uniform " << ((k.tex_shadow & (1u << i)) ? "sampler2DShadow" : dim == kDimFixed3D ? "sampler3D" : dim == kDimFixedCube ? "samplerCube" : "sampler2D") << " u_s" << i << ";\n";
    t << "uniform vec4 u_ec" << i << ";\nuniform vec2 u_es" << i << ";\nuniform float u_tb" << i << ";\nuniform vec2 u_rs" << i << ";\n";
  }
  (void)any_tex;
  if (k.fog) t << "uniform vec4 u_fc;\nuniform vec3 u_fp;\n";
  t << "void main(){\n";
  if (k.two_side) t << "  vec4 prim=gl_FrontFacing?v_c:v_bc; vec3 sec=gl_FrontFacing?v_s:v_bs;\n";
  else t << "  vec4 prim=v_c; vec3 sec=v_s;\n";
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) if (k.tex_dim[i]) write_sample(t, k, i);
  t << "  vec4 prev=prim;\n";
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) if (k.tex_dim[i]) write_env(t, k, i);
  if (k.color_sum) t << "  prev.rgb=clamp(prev.rgb+sec,0.0,1.0);\n";
  if (k.fog) {
    switch (k.fog_mode) {
      case 1: t << "  float ff=(u_fp.z-v_fog)*u_fp.y;\n"; break;      // (end - c) / (end - start)
      case 2: t << "  float ff=exp(-u_fp.x*v_fog);\n"; break;
      default: t << "  float fd=u_fp.x*v_fog; float ff=exp(-fd*fd);\n"; break;
    }
    t << "  prev.rgb=mix(u_fc.rgb,prev.rgb,clamp(ff,0.0,1.0));\n";
  }
  t << "  ff_out=prev;\n}\n";
}

}  // namespace

void GlApi::make_fixed_key(FixedKey* out) const {
  FixedKey& k = *out;
  k = FixedKey();
  k.lighting = fx_.lighting;
  if (fx_.lighting) {
    k.lights = fx_.lights_enabled;
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (!((k.lights >> i) & 1)) continue;
      const Light& l = fx_.lights[i];
      if (l.position[3] != 0) {
        k.light_positional |= 1u << i;
        if (l.spot_cutoff != 180) k.light_spot |= 1u << i;
      }
    }
    k.two_side = fx_.two_side;
    k.local_viewer = fx_.local_viewer;
    k.separate_specular = fx_.separate_specular;
    k.color_material = fx_.color_material;
    k.color_material_face = fx_.color_material_face == GL_FRONT ? 0 : fx_.color_material_face == GL_BACK ? 1 : 2;
    switch (fx_.color_material_mode) {
      case GL_AMBIENT: k.color_material_mode = 0; break;
      case GL_DIFFUSE: k.color_material_mode = 1; break;
      case GL_SPECULAR: k.color_material_mode = 2; break;
      case GL_EMISSION: k.color_material_mode = 4; break;
      default: k.color_material_mode = 3; break;
    }
  }
  k.color_sum = fx_.color_sum || (fx_.lighting && fx_.separate_specular);
  k.fog = fx_.fog;
  if (fx_.fog) {
    k.fog_mode = fx_.fog_mode == GL_LINEAR ? 1 : fx_.fog_mode == GL_EXP ? 2 : 3;
    k.fog_from_coord = fx_.fog_source == GL_FOG_COORD;
  }
  k.clip_mask = fx_.clip_enabled;
  k.normalize = fx_.normalize;
  k.rescale = fx_.rescale_normal;
  k.point_attenuated = fx_.point_atten[0] != 1 || fx_.point_atten[1] != 0 || fx_.point_atten[2] != 0;
  bool normal_gen = false, reflect_gen = false, eye_gen = false;
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
    const FixedState::Unit& u = fx_.units[i];
    uint32_t dim = 0;
    uint32_t engine_id = 0;
    const webgl::TextureUnit& eu = gl_.units_[i];
    if (u.enabled & 8) { dim = kDimFixedCube; engine_id = eu.cube; }
    else if (u.enabled & 4) { dim = kDimFixed3D; engine_id = eu.tex3d; }
    else if (u.enabled & 16) { dim = kDimFixedRect; engine_id = eu.rect; }
    else if (u.enabled & 2) { dim = kDimFixed2D; engine_id = eu.tex2d; }
    else if (u.enabled & 1) { dim = kDimFixed1D; engine_id = eu.tex1d; }
    const webgl::Texture* tex = engine_id ? gl_.texture_at(engine_id) : nullptr;
    if (dim && tex) {
      const webgl::Image* img = nullptr;
      const int32_t base = tex->base_level < 0 ? 0 : tex->base_level;
      const uint64_t at = static_cast<uint64_t>(base);
      if (at < tex->images.count() && tex->images[at] && tex->images[at]->defined()) img = tex->images[at].get();
      if (img) {
        k.tex_dim[i] = static_cast<uint8_t>(dim);
        k.tex_base[i] = static_cast<uint8_t>(base_class(img->info->base));
        if (tex->compare_mode == GL_COMPARE_R_TO_TEXTURE && dim == kDimFixed2D) k.tex_shadow |= 1u << i;
        if (u.env.lod_bias != 0) k.tex_bias |= 1u << i;
      }
    }
    if (k.tex_dim[i]) {
      switch (u.env.mode) {
        case GL_REPLACE: k.tex_mode[i] = kEnvReplace; break;
        case GL_DECAL: k.tex_mode[i] = kEnvDecal; break;
        case GL_BLEND: k.tex_mode[i] = kEnvBlend; break;
        case GL_ADD: k.tex_mode[i] = kEnvAdd; break;
        case GL_COMBINE: {
          k.tex_mode[i] = kEnvCombine;
          FixedKey::Combine& c = k.combine[i];
          c.mode_rgb = static_cast<uint8_t>(combine_code(u.env.combine_rgb));
          c.mode_a = static_cast<uint8_t>(combine_code(u.env.combine_alpha));
          for (int a = 0; a < 3; ++a) {
            c.src_rgb[a] = static_cast<uint8_t>(source_code(u.env.source_rgb[a]));
            c.src_a[a] = static_cast<uint8_t>(source_code(u.env.source_alpha[a]));
            c.op_rgb[a] = static_cast<uint8_t>(u.env.operand_rgb[a] - GL_SRC_COLOR);
            c.op_a[a] = static_cast<uint8_t>(u.env.operand_alpha[a] == GL_ONE_MINUS_SRC_ALPHA ? 1 : 0);
          }
          break;
        }
        default: k.tex_mode[i] = kEnvModulate; break;
      }
      k.tex_needs_coord |= 1u << i;
    }
    if (k.tex_needs_coord & (1u << i)) {
      if (!mat_is_identity(texture_stack_[i])) k.tex_matrix |= 1u << i;
      for (uint32_t c = 0; c < 4; ++c) {
        if (!(u.texgen_enabled & (1u << c))) continue;
        uint32_t m = 0;
        switch (u.gen[c].mode) {
          case GL_OBJECT_LINEAR: m = kGenObject; break;
          case GL_EYE_LINEAR: m = kGenEye; eye_gen = true; break;
          case GL_SPHERE_MAP: m = kGenSphere; reflect_gen = true; normal_gen = true; break;
          case GL_NORMAL_MAP: m = kGenNormal; normal_gen = true; eye_gen = true; break;
          case GL_REFLECTION_MAP: m = kGenReflection; reflect_gen = true; normal_gen = true; break;
          default: break;
        }
        k.texgen[i] |= m << (c * 3);
      }
    }
  }
  k.needs_reflection = reflect_gen;
  k.needs_normal = k.lighting || normal_gen;
  k.needs_eye = k.lighting || eye_gen || reflect_gen || k.clip_mask || k.point_attenuated || (k.fog && !k.fog_from_coord);
}

uint32_t GlApi::compile_program(StringView vs, StringView fs) {
  const uint32_t v = gl_.create_shader(GL_VERTEX_SHADER), f = gl_.create_shader(GL_FRAGMENT_SHADER), p = gl_.create_program();
  gl_.shader_source(v, vs);
  gl_.compile_shader(v);
  gl_.shader_source(f, fs);
  gl_.compile_shader(f);
  gl_.attach_shader(p, v);
  gl_.attach_shader(p, f);
  gl_.bind_attrib_location(p, kAttrVertex, "ff_Vertex");
  gl_.bind_attrib_location(p, kAttrNormal, "ff_Normal");
  gl_.bind_attrib_location(p, kAttrColor, "ff_Color");
  gl_.bind_attrib_location(p, kAttrSecondary, "ff_Color2");
  gl_.bind_attrib_location(p, kAttrFog, "ff_Fog");
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
    char name[8] = {'f', 'f', '_', 'T', 'e', 'x', static_cast<char>('0' + i), 0};
    gl_.bind_attrib_location(p, kAttrTex0 + i, name);
  }
  gl_.link_program(p);
  double ok = 0;
  gl_.program_status(p, GL_LINK_STATUS, &ok);
  gl_.delete_object('s', v);
  gl_.delete_object('s', f);
  return ok != 0 ? p : 0;
}

FixedProgram* GlApi::fixed_program(const FixedKey& key) {
  for (uint64_t i = 0; i < fixed_programs_.count(); ++i) {
    FixedProgram& p = *fixed_programs_[i];
    if (p.key == key) return &p;
  }
  Text vs(a_), fs(a_);
  write_vertex_shader(vs, key);
  write_fragment_shader(fs, key);
  const uint32_t id = compile_program(vs.s.view(), fs.s.view());
  if (id == 0) return nullptr;
  UniquePointer<FixedProgram> made = UniquePointer<FixedProgram>::make(a_);
  if (!made) return nullptr;
  made->key = key;
  made->program = id;
  FixedProgram* result = made.get();
  fixed_programs_.push_back(static_cast<UniquePointer<FixedProgram>&&>(made));

  // Where things are, once.
  gl_.use_program(id);
  auto loc = [&](const char* name) { return gl_.get_uniform_location(id, name); };
  FixedProgram& p = *result;
  p.u_mvp = loc("u_mvp"); p.u_mv = loc("u_mv"); p.u_nm = loc("u_nm"); p.u_nscale = loc("u_nscale"); p.u_ps = loc("u_ps"); p.u_pa = loc("u_pa"); p.u_pr = loc("u_pr");
  p.u_model = loc("u_model"); p.u_fc = loc("u_fc"); p.u_fp = loc("u_fp");
  for (uint32_t f = 0; f < 2; ++f) {
    const char digit = static_cast<char>('0' + f);
    char ma[6] = {'u', '_', 'm', 'a', digit, 0}, md[6] = {'u', '_', 'm', 'd', digit, 0}, ms[6] = {'u', '_', 'm', 's', digit, 0}, me[6] = {'u', '_', 'm', 'e', digit, 0}, sh[8] = {'u', '_', 's', 'h', 'i', 'n', digit, 0};
    p.u_ma[f] = loc(ma); p.u_md[f] = loc(md); p.u_ms[f] = loc(ms); p.u_me[f] = loc(me); p.u_shin[f] = loc(sh);
  }
  auto named = [&](const char* prefix, uint32_t i) {
    Text t(a_);
    t << prefix << i;
    return gl_.get_uniform_location(id, t.s.view());
  };
  for (uint32_t i = 0; i < kMaxLights; ++i) {
    p.u_lA[i] = named("u_lA", i); p.u_lD[i] = named("u_lD", i); p.u_lS[i] = named("u_lS", i); p.u_lp[i] = named("u_lp", i); p.u_la[i] = named("u_la", i);
    p.u_ld[i] = named("u_ld", i); p.u_lc[i] = named("u_lc", i); p.u_le[i] = named("u_le", i);
  }
  for (uint32_t i = 0; i < kMaxClipPlanes; ++i) p.u_cp[i] = named("u_cp", i);
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
    p.u_tm[i] = named("u_tm", i); p.u_s[i] = named("u_s", i); p.u_ec[i] = named("u_ec", i); p.u_es[i] = named("u_es", i); p.u_tb[i] = named("u_tb", i); p.u_rs[i] = named("u_rs", i);
    for (uint32_t c = 0; c < 4; ++c) {
      Text t(a_);
      t << "u_gp" << i << kCoord[c];
      p.u_gp[i][c] = gl_.get_uniform_location(id, t.s.view());
    }
    if (p.u_s[i] >= 0) {
      const uint32_t unit = i;
      gl_.uniform(p.u_s[i], 'i', 1, 1, 1, &unit);
    }
  }
  // The range of the colours among the varyings, and the clip distances.
  const webgl::ProgramObj* po = gl_.program(id);
  const shader::Module& fm = *po->fsm;
  p.flat_first = 0;
  p.flat_count = 0;
  p.clip_first = -1;
  for (const webgl::VaryingLink& l : po->links) {
    for (uint64_t g = 0; g < fm.globals.count(); ++g) {
      const shader::Global& gv = *fm.globals[g];
      if (gv.kind != shader::GlobalKind::In || gv.slot != l.fs_slot) continue;
      const StringView n = gv.name.view();
      if (n.equals("v_c") || n.equals("v_s") || n.equals("v_bc") || n.equals("v_bs")) p.flat_count += l.count;
      if (n.size() > 4 && n.substr(0, 4).equals("v_cl") && (p.clip_first < 0 || static_cast<int32_t>(l.var_offset) < p.clip_first)) p.clip_first = static_cast<int32_t>(l.var_offset);
    }
  }
  return result;
}

void GlApi::load_fixed_uniforms(FixedProgram& p) {
  if (p.generation == fx_generation_) return;
  p.generation = fx_generation_;
  auto f = [&](int32_t loc, uint32_t rows, const float* v) {
    if (loc < 0) return;
    uint32_t lanes[16];
    for (uint32_t i = 0; i < rows; ++i) lanes[i] = __builtin_bit_cast(uint32_t, v[i]);
    gl_.uniform(loc, 'f', 1, rows, 1, lanes);
  };
  auto mat = [&](int32_t loc, uint32_t n, const float* v) {
    if (loc < 0) return;
    uint32_t lanes[16];
    for (uint32_t i = 0; i < n * n; ++i) lanes[i] = __builtin_bit_cast(uint32_t, v[i]);
    gl_.uniform(loc, 'f', n, n, 1, lanes);
  };
  const Mat4& mv = modelview_.entries[modelview_.top];
  const Mat4 mvp = mat_multiply(projection_.entries[projection_.top], mv);
  mat(p.u_mvp, 4, mvp.m);
  mat(p.u_mv, 4, mv.m);
  if (p.u_nm >= 0) {
    Mat4 inv;
    mat_inverse(mv, &inv);
    const float nm[9] = {inv.m[0], inv.m[4], inv.m[8], inv.m[1], inv.m[5], inv.m[9], inv.m[2], inv.m[6], inv.m[10]};
    mat(p.u_nm, 3, nm);
    if (p.u_nscale >= 0) {
      const float len = shader::fsqrt(inv.m[8] * inv.m[8] + inv.m[9] * inv.m[9] + inv.m[10] * inv.m[10]);
      const float scale = len > 0 ? 1.0f / len : 1.0f;
      f(p.u_nscale, 1, &scale);
    }
  }
  f(p.u_ps, 1, &fx_.point_size);
  f(p.u_pa, 3, fx_.point_atten);
  const float range[2] = {fx_.point_min, fx_.point_max};
  f(p.u_pr, 2, range);
  if (p.key.lighting) {
    f(p.u_model, 4, fx_.model_ambient);
    for (uint32_t s = 0; s < 2; ++s) {
      const Material& m = fx_.material[s];
      f(p.u_ma[s], 4, m.ambient); f(p.u_md[s], 4, m.diffuse); f(p.u_ms[s], 4, m.specular); f(p.u_me[s], 4, m.emission); f(p.u_shin[s], 1, &m.shininess);
    }
    for (uint32_t i = 0; i < kMaxLights; ++i) {
      if (!((p.key.lights >> i) & 1)) continue;
      const Light& l = fx_.lights[i];
      f(p.u_lA[i], 4, l.ambient); f(p.u_lD[i], 4, l.diffuse); f(p.u_lS[i], 4, l.specular); f(p.u_lp[i], 4, l.position); f(p.u_la[i], 3, l.attenuation); f(p.u_ld[i], 3, l.spot_direction);
      const float cut = shader::fcos(l.spot_cutoff * 3.14159265358979f / 180.0f);
      f(p.u_lc[i], 1, &cut);
      f(p.u_le[i], 1, &l.spot_exponent);
    }
  }
  if (p.key.fog) {
    f(p.u_fc, 4, fx_.fog_color);
    const float range_inverse = fx_.fog_end != fx_.fog_start ? 1.0f / (fx_.fog_end - fx_.fog_start) : 1.0f;
    const float fp[3] = {fx_.fog_density, range_inverse, fx_.fog_end};
    f(p.u_fp, 3, fp);
  }
  for (uint32_t i = 0; i < kMaxClipPlanes; ++i) {
    if (!(p.key.clip_mask & (1u << i))) continue;
    const float plane[4] = {static_cast<float>(fx_.clip_plane[i][0]), static_cast<float>(fx_.clip_plane[i][1]), static_cast<float>(fx_.clip_plane[i][2]), static_cast<float>(fx_.clip_plane[i][3])};
    f(p.u_cp[i], 4, plane);
  }
  for (uint32_t i = 0; i < kMaxTexUnits; ++i) {
    if (!(p.key.tex_needs_coord & (1u << i))) continue;
    const MatrixStack& ts = texture_stack_[i];
    mat(p.u_tm[i], 4, ts.entries[ts.top].m);
    const FixedState::Unit& u = fx_.units[i];
    f(p.u_ec[i], 4, u.env.color);
    const float es[2] = {u.env.scale_rgb, u.env.scale_alpha};
    f(p.u_es[i], 2, es);
    f(p.u_tb[i], 1, &u.env.lod_bias);
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t mode = (p.key.texgen[i] >> (c * 3)) & 7;
      if (mode == kGenObject) f(p.u_gp[i][c], 4, u.gen[c].object_plane);
      else if (mode == kGenEye) f(p.u_gp[i][c], 4, u.gen[c].eye_plane);
    }
    if (p.u_rs[i] >= 0 && p.key.tex_dim[i] == kDimFixedRect) {
      const webgl::Texture* tex = gl_.texture_at(gl_.units_[i].rect);
      if (tex && tex->images.count() && tex->images[0] && tex->images[0]->defined()) {
        const float rs[2] = {1.0f / static_cast<float>(tex->images[0]->w), 1.0f / static_cast<float>(tex->images[0]->h)};
        f(p.u_rs[i], 2, rs);
      }
    }
  }
}

bool mat_is_identity(const MatrixStack& s) { return is_identity(s.entries[s.top]); }

}  // namespace opengl
