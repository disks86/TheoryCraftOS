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

// Host-side tests of src/Windows/d3d9/ff_lighting.h, which lights the vertices of d3d9.dll's fixed-function pipeline:
//   - its square root, cosine and power against the host's, over their ranges and at the ends;
//   - the matrix for normals;
//   - vertices lit by each kind of light, with the numbers worked out here, and random set-ups against the same sums written with the host's
//     functions;
//   - the coordinates of a texture stage: the vertex's own set through a texture matrix (with the 1 Direct3D 9 puts after the numbers a set
//     has), the counts, projection, and each kind of generated coordinates;
//   - numbers that are no numbers (infinities, NaN) give colours of 0 .. 1 all the same (the test runs under ASan and UBSan).
#include <math.h>
#include <stddef.h>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/Windows/d3d9/ff_lighting.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(x) \
  do { \
    ++g_checks; \
    if (!(x)) { \
      ++g_failed; \
      printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); \
    } \
  } while (0)

static bool near_value(double a, double b, double tolerance = 2e-5) { return fabs(a - b) <= tolerance; }
static unsigned g_seed = 12345;
static float random_float(float low, float high) {
  g_seed = g_seed * 1664525u + 1013904223u;
  return low + (high - low) * (float)(g_seed >> 8) / 16777216.0f;
}

static void identity(FflSetup* s) {
  memset(s, 0, sizeof *s);
  for (int i = 0; i < 4; ++i) s->world_view[i][i] = 1.0f;
  ffl_normal_matrix(s->world_view, s->normal);
}

// The same sums as ffl_light_vertex, with the host's functions and in doubles.
static void reference(const FflSetup* s, const float* position, const float* normal, const float* vd, const float* vs, double diffuse[4], double specular[3]) {
  double p[3], n[3];
  for (int i = 0; i < 3; ++i) {
    p[i] = position[0] * s->world_view[0][i] + position[1] * s->world_view[1][i] + position[2] * s->world_view[2][i] + s->world_view[3][i];
    n[i] = normal[0] * s->normal[0][i] + normal[1] * s->normal[1][i] + normal[2] * s->normal[2][i];
  }
  if (s->normalize) {
    const double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0) for (double& x : n) x /= l;
  }
  double viewer[3] = {0, 0, -1};
  if (s->local_viewer) {
    const double l = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    for (int i = 0; i < 3; ++i) viewer[i] = l > 0 ? -p[i] / l : -p[i];
  }
  double ambient[3] = {s->ambient[0], s->ambient[1], s->ambient[2]}, dsum[3] = {0, 0, 0}, ssum[3] = {0, 0, 0};
  for (unsigned k = 0; k < s->lights; ++k) {
    const FflLight& light = s->light[k];
    double to[3] = {light.to_light[0], light.to_light[1], light.to_light[2]}, weight = 1;
    if (light.type != FFL_DIRECTIONAL) {
      for (int i = 0; i < 3; ++i) to[i] = light.position[i] - p[i];
      const double d = sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
      if (d > 0) for (double& x : to) x /= d;
      if (d > light.range) continue;
      const double divisor = light.attenuation[0] + light.attenuation[1] * d + light.attenuation[2] * d * d;
      if (!(divisor > 0)) continue;
      weight = 1 / divisor;
      if (light.type == FFL_SPOT) {
        const double rho = to[0] * light.to_light[0] + to[1] * light.to_light[1] + to[2] * light.to_light[2];
        if (rho <= light.cos_outer) continue;
        if (rho < light.cos_inner) weight *= pow((rho - light.cos_outer) / (light.cos_inner - light.cos_outer), light.falloff);
      }
    }
    const double facing = n[0] * to[0] + n[1] * to[1] + n[2] * to[2];
    for (int i = 0; i < 3; ++i) ambient[i] += weight * light.ambient[i];
    if (!(facing > 0)) continue;
    for (int i = 0; i < 3; ++i) dsum[i] += weight * facing * light.diffuse[i];
    if (!s->specular) continue;
    double half[3] = {to[0] + viewer[0], to[1] + viewer[1], to[2] + viewer[2]};
    const double hl = sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
    if (hl > 0) for (double& x : half) x /= hl;
    const double shine = n[0] * half[0] + n[1] * half[1] + n[2] * half[2];
    if (!(shine > 0)) continue;
    const double lit = weight * (s->power > 0 ? pow(shine < 1 ? shine : 1, s->power) : 1);
    for (int i = 0; i < 3; ++i) ssum[i] += lit * light.specular[i];
  }
  const float* sources[3] = {nullptr, vd, vs};
  const float* cd = sources[s->diffuse_source] ? sources[s->diffuse_source] : s->material_diffuse;
  const float* ca = sources[s->ambient_source] ? sources[s->ambient_source] : s->material_ambient;
  const float* cs = sources[s->specular_source] ? sources[s->specular_source] : s->material_specular;
  const float* ce = sources[s->emissive_source] ? sources[s->emissive_source] : s->material_emissive;
  auto clamp = [](double x) { return x > 1 ? 1.0 : x > 0 ? x : 0.0; };
  for (int i = 0; i < 3; ++i) {
    diffuse[i] = clamp(ce[i] + ca[i] * ambient[i] + cd[i] * dsum[i]);
    specular[i] = s->specular ? clamp(cs[i] * ssum[i]) : 0;
  }
  diffuse[3] = clamp(cd[3]);
}

static void test_functions() {
  CHECK(ffl_sqrt(0.0f) == 0.0f && ffl_sqrt(-1.0f) == 0.0f && ffl_sqrt(4.0f) == 2.0f && ffl_sqrt(1.0f) == 1.0f);
  for (float x = 1e-30f; x < 1e30f; x *= 1.37f) CHECK(near_value(ffl_sqrt(x) / sqrt((double)x), 1.0, 1e-6));
  CHECK(ffl_sqrt(1e-42f) > 0.0f && near_value(ffl_sqrt(1e-42f) / sqrt((double)1e-42f), 1.0, 1e-5));      // (a denormal)
  CHECK(ffl_sqrt(INFINITY) > 1e19f && ffl_sqrt(NAN) == 0.0f);
  for (float x = 0.0f; x <= 3.1415f; x += 0.001f) CHECK(near_value(ffl_cos(x), cos((double)x), 1e-6));
  CHECK(ffl_cos(-1.0f) == 1.0f && ffl_cos(4.0f) == -1.0f && ffl_cos(NAN) == 1.0f);
  for (float base = 0.001f; base < 1.0f; base += 0.0137f)
    for (float exponent : {0.25f, 0.5f, 1.0f, 2.0f, 3.5f, 8.0f, 20.0f, 64.0f, 128.0f, 1000.0f}) CHECK(near_value(ffl_pow(base, exponent), pow((double)base, (double)exponent), 1e-6));
  CHECK(ffl_pow(0.5f, 0.0f) == 1.0f && ffl_pow(0.0f, 2.0f) == 0.0f && ffl_pow(1.0f, 9.0f) == 1.0f && ffl_pow(2.0f, 3.0f) == 1.0f && ffl_pow(-1.0f, 2.0f) == 0.0f);
  CHECK(ffl_pow(0.5f, 1e30f) == 0.0f && ffl_pow(NAN, 2.0f) == 0.0f && ffl_pow(0.5f, NAN) == 1.0f && ffl_pow(1e-42f, 1.0f) == 0.0f);
  CHECK(near_value(ffl_pow(0.999999f, 1e6f), pow(0.999999f, 1e6), 1e-4));

  // the matrix for normals: a scaling's is the scaling by the inverses, a rotation's the rotation itself, a flat matrix's its own transpose
  float m[4][4] = {{2, 0, 0, 0}, {0, 4, 0, 0}, {0, 0, 0.5f, 0}, {7, 8, 9, 1}}, n[3][3];
  ffl_normal_matrix(m, n);
  CHECK(near_value(n[0][0], 0.5) && near_value(n[1][1], 0.25) && near_value(n[2][2], 2.0) && n[0][1] == 0.0f && n[2][0] == 0.0f);
  const float c = cosf(0.7f), s = sinf(0.7f);
  float r[4][4] = {{c, s, 0, 0}, {-s, c, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  ffl_normal_matrix(r, n);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) CHECK(near_value(n[i][j], r[i][j]));
  float flat[4][4] = {{1, 0, 0, 0}, {2, 0, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  ffl_normal_matrix(flat, n);      // (no inverse: the matrix itself, transposed, as Windows)
  CHECK(ffl_singular(flat) && !ffl_singular(m));
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) CHECK(n[i][j] == flat[j][i]);
}

static void test_lights() {
  FflSetup s;
  const float origin[3] = {0, 0, 2}, towards[3] = {0, 0, -1};
  float diffuse[4], specular[3];
  // nothing shines: the emissive colour and the global ambient on the material's ambient colour; the alpha is the material's
  identity(&s);
  s.ambient[0] = 0.5f, s.ambient[1] = 1.0f;
  s.material_ambient[0] = 0.5f, s.material_ambient[1] = 0.25f, s.material_ambient[2] = 1.0f;
  s.material_emissive[2] = 0.125f;
  s.material_diffuse[3] = 0.75f;
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[0], 0.25) && near_value(diffuse[1], 0.25) && near_value(diffuse[2], 0.125) && near_value(diffuse[3], 0.75));
  CHECK(specular[0] == 0.0f && specular[1] == 0.0f && specular[2] == 0.0f);

  // a directional light at 60 degrees to the normal: half of it
  identity(&s);
  s.material_diffuse[0] = 1.0f, s.material_diffuse[1] = 0.5f, s.material_diffuse[3] = 1.0f;
  s.lights = 1;
  s.light[0].type = FFL_DIRECTIONAL;
  s.light[0].diffuse[0] = s.light[0].diffuse[1] = s.light[0].diffuse[2] = 1.0f;
  s.light[0].to_light[1] = 0.8660254f, s.light[0].to_light[2] = -0.5f;
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[0], 0.5) && near_value(diffuse[1], 0.25) && diffuse[2] == 0.0f && diffuse[3] == 1.0f);
  // ... from behind: nothing; and with no normal nothing
  const float away[3] = {0, 0, 1}, none[3] = {0, 0, 0};
  ffl_light_vertex(&s, origin, away, nullptr, nullptr, diffuse, specular);
  CHECK(diffuse[0] == 0.0f && diffuse[1] == 0.0f);
  ffl_light_vertex(&s, origin, none, nullptr, nullptr, diffuse, specular);
  CHECK(diffuse[0] == 0.0f && diffuse[1] == 0.0f);
  // the diffuse colour from the vertex, its alpha too; from a vertex that has none, the material's
  const float red[4] = {1, 0, 0, 0.25f};
  s.diffuse_source = 1;
  ffl_light_vertex(&s, origin, towards, red, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[0], 0.5) && diffuse[1] == 0.0f && near_value(diffuse[3], 0.25));
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[1], 0.25) && diffuse[3] == 1.0f);
  // a normal that is too long is taken as it is, or brought to length 1
  const float twice[3] = {0, 0, -2};
  s.diffuse_source = 0;
  ffl_light_vertex(&s, origin, twice, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[0], 1.0) && near_value(diffuse[1], 0.5));
  s.normalize = 1;
  ffl_light_vertex(&s, origin, twice, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[0], 0.5) && near_value(diffuse[1], 0.25));

  // a point light 2 in front of the vertex, attenuated by the distance and its square; out of its range, nothing
  identity(&s);
  s.material_diffuse[0] = s.material_diffuse[1] = s.material_diffuse[2] = 1.0f;
  s.lights = 1;
  s.light[0].type = FFL_POINT;
  s.light[0].diffuse[0] = 1.0f, s.light[0].diffuse[1] = 0.5f;
  s.light[0].ambient[2] = 0.5f;
  s.material_ambient[2] = 1.0f;
  s.light[0].range = 10.0f;
  s.light[0].attenuation[0] = 1.0f, s.light[0].attenuation[1] = 0.5f, s.light[0].attenuation[2] = 0.5f;      // 1 + 1 + 2 = 4 at the distance 2
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(diffuse[0], 0.25) && near_value(diffuse[1], 0.125) && near_value(diffuse[2], 0.125));
  s.light[0].range = 1.5f;
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(diffuse[0] == 0.0f && diffuse[2] == 0.0f);
  s.light[0].range = 10.0f;
  s.light[0].attenuation[0] = s.light[0].attenuation[1] = s.light[0].attenuation[2] = 0.0f;      // (nothing to divide by)
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(diffuse[0] == 0.0f);

  // a spotlight at the origin that points along z: all inside the inner cone, none outside the outer one, between them by the falloff
  identity(&s);
  s.material_diffuse[0] = 1.0f;
  s.lights = 1;
  s.light[0].type = FFL_SPOT;
  s.light[0].diffuse[0] = 1.0f;
  s.light[0].to_light[2] = -1.0f;
  s.light[0].range = 100.0f;
  s.light[0].attenuation[0] = 1.0f;
  s.light[0].cos_inner = ffl_cos(0.2f), s.light[0].cos_outer = ffl_cos(0.6f);
  s.light[0].falloff = 2.0f;
  s.normalize = 1;
  for (float angle : {0.0f, 0.1f, 0.19f, 0.3f, 0.45f, 0.59f, 0.61f, 1.0f}) {
    const float at[3] = {sinf(angle) * 3.0f, 0, cosf(angle) * 3.0f}, back[3] = {-at[0], 0, -at[2]};      // (the normal looks at the light)
    ffl_light_vertex(&s, at, back, nullptr, nullptr, diffuse, specular);
    const double between = (cos(angle) - cos(0.6)) / (cos(0.2) - cos(0.6));
    const double expected = angle <= 0.2f ? 1.0 : angle >= 0.6f ? 0.0 : between * between;
    CHECK(near_value(diffuse[0], expected, 1e-4));
  }

  // the specular colour: light straight at the vertex, the camera straight behind the light - the whole of it; off to a side, a power of the cosine
  identity(&s);
  s.specular = 1;
  s.material_specular[0] = 1.0f, s.material_specular[1] = 0.5f;
  s.power = 4.0f;
  s.lights = 1;
  s.light[0].type = FFL_DIRECTIONAL;
  s.light[0].specular[0] = s.light[0].specular[1] = s.light[0].specular[2] = 1.0f;
  s.light[0].to_light[2] = -1.0f;
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(specular[0], 1.0) && near_value(specular[1], 0.5) && specular[2] == 0.0f && diffuse[0] == 0.0f);
  s.light[0].to_light[0] = 0.6f, s.light[0].to_light[2] = -0.8f;      // half way: (0.6, 0, -1.8) / its length; its z is the cosine
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(near_value(specular[0], pow(1.8 / sqrt(0.36 + 3.24), 4.0), 1e-5));
  // the camera's direction is the vertex's own with a local viewer, which differs off the axis
  const float aside[3] = {2, 0, 2};
  float plain[3], local[3];
  ffl_light_vertex(&s, aside, towards, nullptr, nullptr, diffuse, plain);
  s.local_viewer = 1;
  ffl_light_vertex(&s, aside, towards, nullptr, nullptr, diffuse, local);
  CHECK(!near_value(plain[0], local[0], 1e-3));
  s.specular = 0;      // (not asked for: none)
  ffl_light_vertex(&s, origin, towards, nullptr, nullptr, diffuse, specular);
  CHECK(specular[0] == 0.0f);

  // random set-ups against the sums with the host's functions
  unsigned compared = 0;
  for (int round = 0; round < 4000; ++round) {
    identity(&s);
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) s.world_view[i][j] = random_float(-1.5f, 1.5f);
    for (int j = 0; j < 3; ++j) s.world_view[3][j] = random_float(-3, 3);
    ffl_normal_matrix(s.world_view, s.normal);
    for (int i = 0; i < 3; ++i) {
      s.ambient[i] = random_float(0, 0.3f);
      s.material_diffuse[i] = random_float(0, 1), s.material_ambient[i] = random_float(0, 1), s.material_specular[i] = random_float(0, 1);
      s.material_emissive[i] = random_float(0, 0.2f);
    }
    s.material_diffuse[3] = random_float(0, 1);
    s.power = random_float(0, 40);
    s.diffuse_source = round % 3, s.ambient_source = (round / 3) % 3, s.specular_source = (round / 9) % 3, s.emissive_source = (round / 27) % 3;
    s.specular = round & 1, s.normalize = (round >> 1) & 1, s.local_viewer = (round >> 2) & 1;
    s.lights = 1 + (unsigned)round % FFL_LIGHTS;
    for (unsigned k = 0; k < s.lights; ++k) {
      FflLight& light = s.light[k];
      light.type = (int)((unsigned)(round + k) % 3);
      for (int i = 0; i < 3; ++i) {
        light.diffuse[i] = random_float(0, 0.5f), light.specular[i] = random_float(0, 0.5f), light.ambient[i] = random_float(0, 0.1f);
        light.position[i] = random_float(-5, 5), light.to_light[i] = random_float(-1, 1);
      }
      ffl_normalize(light.to_light);
      light.range = random_float(1, 12);
      light.attenuation[0] = random_float(0, 1), light.attenuation[1] = random_float(0, 0.5f), light.attenuation[2] = random_float(0, 0.2f);
      const float inner = random_float(0, 1.5f);
      light.cos_inner = ffl_cos(inner), light.cos_outer = ffl_cos(inner + random_float(0.05f, 1.5f));
      light.falloff = random_float(0, 3);
    }
    const float position[3] = {random_float(-2, 2), random_float(-2, 2), random_float(-2, 2)}, normal[3] = {random_float(-1, 1), random_float(-1, 1), random_float(-1, 1)};
    const float vd[4] = {random_float(0, 1), random_float(0, 1), random_float(0, 1), random_float(0, 1)}, vs[4] = {random_float(0, 1), random_float(0, 1), random_float(0, 1), 1};
    const bool has_diffuse = (round >> 3) & 1, has_specular = (round >> 4) & 1;
    double want_diffuse[4], want_specular[3];
    reference(&s, position, normal, has_diffuse ? vd : nullptr, has_specular ? vs : nullptr, want_diffuse, want_specular);
    ffl_light_vertex(&s, position, normal, has_diffuse ? vd : nullptr, has_specular ? vs : nullptr, diffuse, specular);
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && near_value(diffuse[i], want_diffuse[i], 2e-3);
    for (int i = 0; i < 3; ++i) same = same && near_value(specular[i], want_specular[i], 2e-3);
    CHECK(same);
    ++compared;
  }
  CHECK(compared == 4000);

  // numbers that are no numbers: the colours are still 0 .. 1, whatever is where
  const float bad[] = {INFINITY, -INFINITY, NAN, 1e38f, -1e38f, 1e-45f, 0.0f};
  unsigned inside = 0, cases = 0;
  for (int round = 0; round < 3000; ++round) {
    identity(&s);
    s.specular = 1, s.normalize = round & 1, s.local_viewer = (round >> 1) & 1;
    s.lights = FFL_LIGHTS;
    float* fields = (float*)&s;
    const size_t count = sizeof s / sizeof(float);
    for (int k = 0; k < 6; ++k) {
      g_seed = g_seed * 1664525u + 1013904223u;
      const size_t at = (g_seed >> 8) % count;
      // (the whole numbers of the set-up are left alone: the kinds, the sources, the counts)
      const size_t offset = at * sizeof(float);
      if ((offset >= offsetof(FflSetup, diffuse_source) && offset < offsetof(FflSetup, light)) || (offset >= offsetof(FflSetup, light) && (offset - offsetof(FflSetup, light)) % sizeof(FflLight) == 0)) continue;
      fields[at] = bad[(g_seed >> 3) % 7];
    }
    for (unsigned k = 0; k < FFL_LIGHTS; ++k) s.light[k].type = (int)((round + k) % 3);
    const float position[3] = {bad[round % 7], 1, 2}, normal[3] = {0, bad[(round / 7) % 7], -1}, vd[4] = {bad[(round / 49) % 7], 0.5f, 0.5f, bad[round % 7]};
    s.diffuse_source = round % 3;
    ffl_light_vertex(&s, position, normal, vd, vd, diffuse, specular);
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok = ok && diffuse[i] >= 0.0f && diffuse[i] <= 1.0f;
    for (int i = 0; i < 3; ++i) ok = ok && specular[i] >= 0.0f && specular[i] <= 1.0f;
    inside += ok;
    ++cases;
  }
  CHECK(inside == cases);
  printf("  %u random set-ups compared, %u with numbers that are none\n", compared, cases);
}

static void test_texture_coordinates() {
  FflSetup s;
  identity(&s);
  FflTextureStage stage;
  memset(&stage, 0, sizeof stage);
  for (int i = 0; i < 4; ++i) stage.matrix[i][i] = 1.0f;
  const float position[3] = {1, 2, 4}, normal[3] = {0, 0, -1};
  const float uv[4] = {0.25f, 0.75f, 0, 1};
  float out[4];
  auto run = [&](const float* vertex, int components, int for_program) {
    ffl_texture_coordinates(&stage, s.world_view, s.normal, s.normalize, position, normal, vertex, components, for_program, out);
  };
  // no matrix: the set as it is; for a program's pixel shader what the set has not is 0 and 1
  run(uv, 2, 0);
  CHECK(out[0] == 0.25f && out[1] == 0.75f && out[2] == 0.0f && out[3] == 1.0f);
  const float uvw[4] = {0.25f, 0.75f, 0.5f, 1};
  run(uvw, 3, 1);
  CHECK(out[0] == 0.25f && out[1] == 0.75f && out[2] == 0.5f && out[3] == 1.0f);
  run(uvw, 3, 0);
  CHECK(out[2] == 0.5f && out[3] == 1.0f);      // (the third number reaches the stages too: cube and volume textures are sampled by it)
  const float nothing[4] = {0, 0, 0, 1};
  run(nothing, 0, 0);
  CHECK(out[0] == 0.0f && out[1] == 0.0f && out[3] == 1.0f);
  // a matrix on two numbers: they are (u, v, 1, 0), so the third row moves them and the fourth does nothing
  stage.count = 2;
  stage.matrix[0][0] = 2.0f;
  stage.matrix[2][0] = 0.5f, stage.matrix[2][1] = -0.25f;
  stage.matrix[3][0] = 100.0f, stage.matrix[3][1] = 100.0f;
  run(uv, 2, 0);
  CHECK(near_value(out[0], 1.0) && near_value(out[1], 0.5) && out[2] == 0.0f && out[3] == 1.0f);
  // ... on three, the fourth row does; on one, the second
  run(uvw, 3, 0);
  CHECK(near_value(out[0], 0.25 * 2 + 0.5 * 0.5 + 100) && near_value(out[1], 0.75 - 0.5 * 0.25 + 100));
  stage.matrix[1][0] = 7.0f;
  run(uv, 1, 0);
  CHECK(near_value(out[0], 0.25 * 2 + 7));
  stage.matrix[1][0] = 0.0f;
  // the count: one number leaves v 0; for a program the numbers past the count are 0 and 1
  stage.count = 1;
  run(uv, 2, 0);
  CHECK(near_value(out[0], 1.0) && out[1] == 0.0f && out[3] == 1.0f);
  stage.count = 3;
  stage.matrix[2][2] = 3.0f;
  run(uv, 2, 1);
  CHECK(near_value(out[0], 1.0) && near_value(out[1], 0.5) && near_value(out[2], 3.0) && out[3] == 1.0f);
  stage.count = 9;      // (more than there are: four)
  run(uv, 2, 1);
  CHECK(near_value(out[2], 3.0) && out[3] == 0.0f);
  // projected: the pixel stage divides by the last of the numbers; a program does that itself
  stage.count = 3, stage.projected = 1;
  run(uv, 2, 0);
  CHECK(near_value(out[0], 1.0) && near_value(out[1], 0.5) && out[2] == 0.0f && near_value(out[3], 3.0));
  stage.count = 4;
  stage.matrix[0][3] = 4.0f;
  run(uv, 2, 0);
  CHECK(near_value(out[3], 1.0));      // (u * 4)
  stage.count = 2;
  run(uv, 2, 0);
  CHECK(near_value(out[3], out[1]));
  run(uv, 2, 1);
  CHECK(out[3] == 1.0f);

  // generated coordinates, through a world * view that moves by (0, 0, 1) and doubles x
  memset(&stage, 0, sizeof stage);
  for (int i = 0; i < 4; ++i) stage.matrix[i][i] = 1.0f;
  s.world_view[0][0] = 2.0f;
  s.world_view[3][2] = 1.0f;
  ffl_normal_matrix(s.world_view, s.normal);
  stage.from = FFL_FROM_POSITION;
  run(uv, 2, 1);
  CHECK(near_value(out[0], 2.0) && near_value(out[1], 2.0) && near_value(out[2], 5.0) && out[3] == 1.0f);
  stage.count = 3;      // three numbers: the fourth row moves them
  stage.matrix[3][2] = -5.0f;
  run(uv, 2, 1);
  CHECK(near_value(out[2], 0.0));
  stage.count = 0;
  stage.from = FFL_FROM_NORMAL;
  const float slanted[3] = {2, 0, -2};
  ffl_texture_coordinates(&stage, s.world_view, s.normal, 0, position, slanted, uv, 2, 1, out);
  CHECK(near_value(out[0], 1.0) && near_value(out[1], 0.0) && near_value(out[2], -2.0));      // (x through the inverse of the doubling)
  ffl_texture_coordinates(&stage, s.world_view, s.normal, 1, position, slanted, uv, 2, 1, out);
  CHECK(near_value(out[0], 1 / sqrt(5.0)) && near_value(out[2], -2 / sqrt(5.0)));
  // the reflection: straight at a wall that faces the camera it comes straight back; at a slanted one it goes off to the side
  identity(&s);
  stage.from = FFL_FROM_REFLECTION;
  const float ahead[3] = {0, 0, 3};
  ffl_texture_coordinates(&stage, s.world_view, s.normal, 1, ahead, normal, uv, 2, 1, out);
  CHECK(near_value(out[0], 0.0) && near_value(out[1], 0.0) && near_value(out[2], -1.0));
  const float diagonal[3] = {-0.70710678f, 0, -0.70710678f};
  ffl_texture_coordinates(&stage, s.world_view, s.normal, 1, ahead, diagonal, uv, 2, 1, out);
  CHECK(near_value(out[0], -1.0) && near_value(out[1], 0.0) && near_value(out[2], 0.0, 1e-4));
  // the sphere map: the reflection straight back is the middle of the map; (-1, 0, 0) is r / (2 sqrt 2) left of it
  stage.from = FFL_FROM_SPHERE;
  ffl_texture_coordinates(&stage, s.world_view, s.normal, 1, ahead, diagonal, uv, 2, 0, out);
  CHECK(near_value(out[0], 0.5 - 1 / (2 * sqrt(2.0)), 1e-4) && near_value(out[1], 0.5) && out[3] == 1.0f);
  const float behind[3] = {0, 0, 1};      // (a wall that faces away: the reflection goes on along z, where the map has no place - its middle)
  const float sideways[3] = {1, 0, 0};
  ffl_texture_coordinates(&stage, s.world_view, s.normal, 1, ahead, sideways, uv, 2, 0, out);
  CHECK(out[0] >= 0.0f && out[0] <= 1.0f && out[1] == 0.5f);
  (void)behind;

  // numbers that are none do no harm (nothing here reads outside its arrays whatever they are)
  const float bad[] = {INFINITY, -INFINITY, NAN, 1e38f, -1e38f, 1e-45f, 0.0f};
  unsigned cases = 0;
  for (int round = 0; round < 2000; ++round) {
    stage.from = round % 5;
    stage.count = round % 7 - 1;
    stage.projected = round & 1;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) stage.matrix[i][j] = (round + i * 4 + j) % 5 == 0 ? bad[(round + i + j) % 7] : random_float(-2, 2);
    const float at[3] = {bad[round % 7], 1, bad[(round / 7) % 7]}, n[3] = {bad[(round / 3) % 7], 0, -1}, set[4] = {bad[(round / 5) % 7], 0.5f, 0.5f, 1};
    ffl_texture_coordinates(&stage, s.world_view, s.normal, round & 1, at, n, set, round % 6 - 1, (round >> 1) & 1, out);
    ++cases;
  }
  CHECK(cases == 2000);
}

int main() {
  test_functions();
  test_lights();
  test_texture_coordinates();
  if (g_failed) {
    printf("FfLightingTest: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  printf("FfLightingTest: all %d checks passed\n", g_checks);
  return 0;
}
