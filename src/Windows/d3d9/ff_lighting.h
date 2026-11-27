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

/* The lighting of Direct3D 9's fixed-function pipeline, for one vertex at a time: d3d9.dll lights the vertices where it decodes them
 * (draw.inc), so the two shaders of the pipeline stay what they are. Nothing here needs a library - the DLL has no C runtime - so the
 * square root, the power and the cosine are written out; tests/FfLightingTest.cpp holds them against the host's.
 *
 * What Direct3D 9 says a lit vertex is, in view space (the camera at the origin, looking along +z):
 *
 *   diffuse colour  = emissive + ambient colour * (the global ambient + the sum of the lights' ambients)
 *                     + diffuse colour * the sum of the lights' diffuse * max(N . L, 0)
 *   specular colour = specular colour * the sum of the lights' specular * (N . H) ^ power      (where N . L > 0)
 *
 * each light's share weighed by its attenuation (1 / (a0 + a1 d + a2 d^2) inside its range, for the lights that have a place) and, for a
 * spotlight, by where the vertex is in its cone. N is the normal, L the direction to the light, H half way between L and the direction
 * to the camera. The alpha is the diffuse colour's. Both results are clamped to 0 .. 1.
 *
 * The texture coordinates of a stage are made here too (ffl_texture_coordinates): the vertex's own set or one generated from its place in
 * view space, through the stage's texture matrix. */
#ifndef TCOS_D3D9_FF_LIGHTING_H
#define TCOS_D3D9_FF_LIGHTING_H

#define FFL_LIGHTS 32u     /* the lights that shine at once (D3DCAPS9.MaxActiveLights) */

enum { FFL_DIRECTIONAL = 0, FFL_POINT = 1, FFL_SPOT = 2 };

typedef struct FflLight {
  int type;
  float diffuse[3], specular[3], ambient[3];
  float position[3];        /* in view space (point and spot) */
  float to_light[3];        /* a directional light: the direction to it, of length 1; a spotlight: against the way it points */
  float range, attenuation[3];
  float cos_inner, cos_outer, falloff;      /* a spotlight: the cosines of half theta and half phi */
} FflLight;

typedef struct FflSetup {
  float world_view[4][4];   /* positions into view space: a row vector times this */
  float normal[3][3];       /* normals into view space: a row vector times this (the inverse of world_view's 3 x 3, transposed) */
  float ambient[3];         /* the global ambient */
  float material_diffuse[4], material_ambient[3], material_specular[3], material_emissive[3], power;
  /* where each colour of the material comes from: 0 the material, 1 the vertex's diffuse colour, 2 its specular colour */
  int diffuse_source, ambient_source, specular_source, emissive_source;
  int specular;             /* the specular colour is worked out */
  int normalize;            /* normals are brought to length 1 */
  int local_viewer;         /* the direction to the camera is the vertex's own, not straight back along z */
  unsigned lights;
  FflLight light[FFL_LIGHTS];
} FflSetup;

/* The square root: Newton's steps from a guess that halves the exponent. 0 for what is not above 0. */
static float ffl_sqrt(float x) {
  if (!(x > 0.0f)) return 0.0f;
  if (x > 3.0e38f) return 1.8446743e19f;      /* (infinity, or next to it) */
  union { float f; unsigned u; } guess;
  guess.f = x;
  guess.u = (guess.u >> 1) + 0x1FC00000u;
  double y = guess.f;
  for (int i = 0; i < 40; ++i) {      /* (a handful of steps; more only from the poor guess a denormal gives) */
    const double next = 0.5 * (y + (double)x / y);
    if (next == y) break;
    y = next;
  }
  return (float)y;
}

/* The cosine, for the half angles of a spotlight's cone: of 0 .. pi and a little more; angles outside count as the nearest end. */
static float ffl_cos(float x) {
  if (!(x > 0.0f)) return 1.0f;
  if (x > 3.14159265f) return -1.0f;
  int negative = 0;
  double t = x;
  if (t > 1.5707963267948966) {      /* cos(x) = -cos(pi - x) */
    t = 3.141592653589793 - t;
    negative = 1;
  }
  const double s = t * t;
  const double c = 1.0 + s * (-1.0 / 2 + s * (1.0 / 24 + s * (-1.0 / 720 + s * (1.0 / 40320 + s * (-1.0 / 3628800 + s * (1.0 / 479001600))))));
  return (float)(negative ? -c : c);
}

/* base ^ exponent for a base of 0 .. 1 (a cosine) and an exponent that is not negative, as 2 ^ (exponent * log2(base)). */
static float ffl_pow(float base, float exponent) {
  if (!(exponent > 0.0f)) return 1.0f;
  if (!(base > 0.0f)) return 0.0f;
  if (base >= 1.0f) return 1.0f;
  /* log2: the exponent of the number, and a series for its mantissa m in [sqrt(1/2), sqrt(2)): ln m = 2 atanh((m - 1) / (m + 1)) */
  union { float f; unsigned u; } bits;
  bits.f = base;
  int e = (int)(bits.u >> 23) - 127;
  if (e < -126) return 0.0f;      /* (a denormal: nothing a colour would show) */
  bits.u = (bits.u & 0x007FFFFFu) | 0x3F800000u;
  double m = bits.f;
  if (m > 1.4142135623730951) {
    m *= 0.5;
    ++e;
  }
  const double z = (m - 1.0) / (m + 1.0), z2 = z * z;
  const double ln = 2.0 * z * (1.0 + z2 * (1.0 / 3 + z2 * (1.0 / 5 + z2 * (1.0 / 7 + z2 * (1.0 / 9 + z2 * (1.0 / 11))))));
  const double power = (double)exponent * ((double)e + ln * 1.4426950408889634);
  if (!(power > -150.0)) return 0.0f;
  /* 2 ^ power: the whole part by halving, the rest by the series of e ^ (rest * ln 2), rest in [-1/2, 1/2] */
  int whole = (int)power;
  if ((double)whole > power) --whole;
  double rest = power - (double)whole;
  if (rest > 0.5) {
    rest -= 1.0;
    ++whole;
  }
  const double a = rest * 0.6931471805599453;
  double result = 1.0 + a * (1.0 + a * (1.0 / 2 + a * (1.0 / 6 + a * (1.0 / 24 + a * (1.0 / 120 + a * (1.0 / 720 + a * (1.0 / 5040)))))));
  for (; whole < 0; ++whole) result *= 0.5;
  return (float)result;
}

static float ffl_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
/* Brings a vector to length 1 and gives the length it had; a vector of no length stays as it is. */
static float ffl_normalize(float v[3]) {
  const float length = ffl_sqrt(ffl_dot(v, v));
  if (length > 0.0f) {
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
  }
  return length;
}
static float ffl_clamp(float x) { return x > 1.0f ? 1.0f : x > 0.0f ? x : 0.0f; }      /* (not a number: 0) */

/* Whether the 3 x 3 of a matrix for positions has no inverse (its determinant is 0, or as good as). */
static int ffl_singular(const float m[4][4]) {
  const float determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                            m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  return !(determinant > 1e-30f || determinant < -1e-30f);
}
/* The matrix for normals out of the one for positions: the inverse of its 3 x 3, transposed - which is its cofactors over its determinant.
 * A matrix that has no inverse is used as it is in the inverse's place (its 3 x 3 transposed): Windows does so - Wine's test_lighting of
 * ddraw 1-7 and d3d9 light a quad under a singular world matrix (and wined3d does the same). */
static void ffl_normal_matrix3(const float m[4][4], float out[3][3]);
static void ffl_normal_matrix(const float m[4][4], float out[3][3]) {
  /* A matrix with a fourth column other than (0, 0, 0, 1) (not affine) takes the 3 x 3 of its whole 4 x 4 inverse, transposed, as Windows: Wine's
   * test_lighting of ddraw 7 ("non-affine matrix") records black there, which only that inverse gives. (For an affine matrix it is the same as the
   * inverse of the 3 x 3.) */
  if (m[0][3] != 0.0f || m[1][3] != 0.0f || m[2][3] != 0.0f || m[3][3] != 1.0f) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? (double)m[i][j] : (double)(j - 4 == i);
    int ok = 1;
    for (int c = 0; c < 4 && ok; ++c) {
      int p = c;
      for (int r = c + 1; r < 4; ++r)
        if ((a[r][c] < 0 ? -a[r][c] : a[r][c]) > (a[p][c] < 0 ? -a[p][c] : a[p][c])) p = r;
      if (!(a[p][c] > 1e-30 || a[p][c] < -1e-30)) { ok = 0; break; }
      if (p != c)
        for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
      const double d = a[c][c];
      for (int j = 0; j < 8; ++j) a[c][j] /= d;
      for (int r = 0; r < 4; ++r) {
        if (r == c || a[r][c] == 0.0) continue;
        const double f = a[r][c];
        for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
      }
    }
    if (ok) {
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = (float)a[j][4 + i];
      return;
    }
  }
  ffl_normal_matrix3(m, out);
}
/* The same from the 3 x 3 alone, whatever the fourth column (Direct3D 1-3's lighting: ddraw 1/2/4 test_lighting light the non-affine quad). */
static void ffl_normal_matrix3(const float m[4][4], float out[3][3]) {
  float cofactor[3][3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
      cofactor[i][j] = m[i1][j1] * m[i2][j2] - m[i1][j2] * m[i2][j1];
    }
  }
  const float determinant = m[0][0] * cofactor[0][0] + m[0][1] * cofactor[0][1] + m[0][2] * cofactor[0][2];
  const int usable = determinant > 1e-30f || determinant < -1e-30f;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out[i][j] = usable ? cofactor[i][j] / determinant : m[j][i];
}

/* Lights one vertex. `position` and `normal` are the vertex's own (object space); `vertex_diffuse` and `vertex_specular` its two colours, or
 * NULL for one it has not - a colour of the material that would come from it comes from the material then. `diffuse` gets the lit colour
 * (four numbers), `specular` the specular one (three). */
static void ffl_light_vertex(const FflSetup *s, const float position[3], const float normal[3], const float *vertex_diffuse, const float *vertex_specular,
                             float diffuse[4], float specular[3]) {
  float place[3], n[3];
  for (int i = 0; i < 3; ++i) {
    place[i] = position[0] * s->world_view[0][i] + position[1] * s->world_view[1][i] + position[2] * s->world_view[2][i] + s->world_view[3][i];
    n[i] = normal[0] * s->normal[0][i] + normal[1] * s->normal[1][i] + normal[2] * s->normal[2][i];
  }
  if (s->normalize) ffl_normalize(n);
  float to_viewer[3] = {0.0f, 0.0f, -1.0f};
  if (s->local_viewer) {
    for (int i = 0; i < 3; ++i) to_viewer[i] = -place[i];
    ffl_normalize(to_viewer);
  }

  float ambient_sum[3] = {s->ambient[0], s->ambient[1], s->ambient[2]}, diffuse_sum[3] = {0.0f, 0.0f, 0.0f}, specular_sum[3] = {0.0f, 0.0f, 0.0f};
  for (unsigned k = 0; k < s->lights && k < FFL_LIGHTS; ++k) {
    const FflLight *light = &s->light[k];
    float to_light[3] = {light->to_light[0], light->to_light[1], light->to_light[2]}, weight = 1.0f;
    if (light->type != FFL_DIRECTIONAL) {
      for (int i = 0; i < 3; ++i) to_light[i] = light->position[i] - place[i];
      const float distance = ffl_normalize(to_light);
      if (distance > light->range) continue;
      const float divisor = light->attenuation[0] + light->attenuation[1] * distance + light->attenuation[2] * distance * distance;
      if (!(divisor > 0.0f)) continue;      /* (nothing to divide by: the light does not reach) */
      weight = 1.0f / divisor;
      if (light->type == FFL_SPOT) {
        /* how far off the light's axis the vertex is, as the cosine: all the light inside the inner cone, none outside the outer one */
        const float rho = ffl_dot(to_light, light->to_light);
        if (rho <= light->cos_outer) continue;
        if (rho < light->cos_inner) weight *= ffl_pow((rho - light->cos_outer) / (light->cos_inner - light->cos_outer), light->falloff);
      }
    }
    const float facing = ffl_dot(n, to_light);
    for (int i = 0; i < 3; ++i) ambient_sum[i] += weight * light->ambient[i];
    if (!(facing > 0.0f)) continue;
    for (int i = 0; i < 3; ++i) diffuse_sum[i] += weight * facing * light->diffuse[i];
    if (!s->specular) continue;
    float half[3] = {to_light[0] + to_viewer[0], to_light[1] + to_viewer[1], to_light[2] + to_viewer[2]};
    ffl_normalize(half);
    const float shine = ffl_dot(n, half);
    if (!(shine > 0.0f)) continue;
    const float lit = weight * ffl_pow(shine, s->power);
    for (int i = 0; i < 3; ++i) specular_sum[i] += lit * light->specular[i];
  }

  /* the colours of the material, each from where the render states say: the material itself, or one of the vertex's colours if it has it */
  const float *sources[3] = {NULL, vertex_diffuse, vertex_specular};
  const float *from_diffuse = s->diffuse_source >= 0 && s->diffuse_source <= 2 ? sources[s->diffuse_source] : NULL;
  const float *from_ambient = s->ambient_source >= 0 && s->ambient_source <= 2 ? sources[s->ambient_source] : NULL;
  const float *from_specular = s->specular_source >= 0 && s->specular_source <= 2 ? sources[s->specular_source] : NULL;
  const float *from_emissive = s->emissive_source >= 0 && s->emissive_source <= 2 ? sources[s->emissive_source] : NULL;
  const float *cd = from_diffuse ? from_diffuse : s->material_diffuse;
  const float *ca = from_ambient ? from_ambient : s->material_ambient;
  const float *cs = from_specular ? from_specular : s->material_specular;
  const float *ce = from_emissive ? from_emissive : s->material_emissive;
  for (int i = 0; i < 3; ++i) {
    diffuse[i] = ffl_clamp(ce[i] + ca[i] * ambient_sum[i] + cd[i] * diffuse_sum[i]);
    specular[i] = s->specular ? ffl_clamp(cs[i] * specular_sum[i]) : 0.0f;
  }
  diffuse[3] = ffl_clamp(cd[3]);
}

/* ---- texture coordinates ------------------------------------------------------------------------------------------------------------------------------ */

/* Where a stage's coordinates come from. */
enum {
  FFL_FROM_VERTEX = 0,     /* a set of the vertex's */
  FFL_FROM_NORMAL = 1,     /* the normal in view space (D3DTSS_TCI_CAMERASPACENORMAL) */
  FFL_FROM_POSITION = 2,   /* the position in view space (D3DTSS_TCI_CAMERASPACEPOSITION) */
  FFL_FROM_REFLECTION = 3, /* the direction from the camera, mirrored at the normal (D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR) */
  FFL_FROM_SPHERE = 4      /* ... as the place in a sphere map (D3DTSS_TCI_SPHEREMAP) */
};

typedef struct FflTextureStage {
  int from;                /* FFL_FROM_* */
  int count;               /* 0: no texture matrix; else the numbers the matrix gives, 1 .. 4 (D3DTTFF_COUNT1 ..) */
  int projected;           /* the coordinates are divided by the last of those numbers (D3DTTFF_PROJECTED) */
  float matrix[4][4];      /* a row vector times this */
} FflTextureStage;

/* The coordinates of one stage for one vertex. `vertex` is the set the stage reads, as four numbers, of which the vertex has `components`
 * (0: it has no such set); `world_view` and `normal_matrix` are the ones of FflSetup, read only where coordinates are generated.
 *
 * Direct3D 9 puts a 1 after the numbers a set has before the matrix is applied - (u, v) is (u, v, 1, 0) - so a matrix moves two numbers by
 * its third row. Generated coordinates are three numbers.
 *
 * `out` for the fixed-function pixel stage (`for_program` 0) is u, v, 0 and the number the pixel stage divides u and v by (1 unless the
 * coordinates are projected); for a program's pixel shader it is the numbers as they are, 0 0 0 1 where there are none. */
static void ffl_texture_coordinates(const FflTextureStage *stage, const float world_view[4][4], const float normal_matrix[3][3], int normalize, const float position[3],
                                    const float normal[3], const float *vertex, int components, int for_program, float out[4]) {
  float in[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  int have = 3;
  if (stage->from == FFL_FROM_VERTEX) {
    have = components < 0 ? 0 : components > 4 ? 4 : components;
    for (int i = 0; i < have; ++i) in[i] = vertex[i];
  } else {
    float place[3], n[3];
    for (int i = 0; i < 3; ++i) {
      place[i] = position[0] * world_view[0][i] + position[1] * world_view[1][i] + position[2] * world_view[2][i] + world_view[3][i];
      n[i] = normal[0] * normal_matrix[0][i] + normal[1] * normal_matrix[1][i] + normal[2] * normal_matrix[2][i];
    }
    if (normalize) ffl_normalize(n);
    if (stage->from == FFL_FROM_NORMAL) {
      for (int i = 0; i < 3; ++i) in[i] = n[i];
    } else if (stage->from == FFL_FROM_POSITION) {
      for (int i = 0; i < 3; ++i) in[i] = place[i];
    } else {
      /* the way from the camera to the vertex, mirrored at the normal: I - 2 (N . I) N */
      float way[3] = {place[0], place[1], place[2]}, mirrored[3];
      ffl_normalize(way);
      const float along = 2.0f * ffl_dot(n, way);
      for (int i = 0; i < 3; ++i) mirrored[i] = way[i] - along * n[i];
      if (stage->from == FFL_FROM_REFLECTION) {
        for (int i = 0; i < 3; ++i) in[i] = mirrored[i];
      } else {
        const float m = 2.0f * ffl_sqrt(mirrored[0] * mirrored[0] + mirrored[1] * mirrored[1] + (mirrored[2] + 1.0f) * (mirrored[2] + 1.0f));
        in[0] = m > 0.0f ? mirrored[0] / m + 0.5f : 0.5f;
        in[1] = m > 0.0f ? mirrored[1] / m + 0.5f : 0.5f;
        in[2] = 0.0f;
      }
    }
  }
  if (stage->count <= 0) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = for_program ? in[2] : 0.0f;
    out[3] = for_program ? in[3] : 1.0f;
    return;
  }
  /* (the 1 after the numbers there are; three numbers have it where it is already) */
  if (have < 3) {
    in[have] = 1.0f;
    in[3] = 0.0f;
  }
  const int count = stage->count > 4 ? 4 : stage->count;
  float moved[4];
  for (int i = 0; i < 4; ++i) moved[i] = in[0] * stage->matrix[0][i] + in[1] * stage->matrix[1][i] + in[2] * stage->matrix[2][i] + in[3] * stage->matrix[3][i];
  if (for_program) {
    for (int i = 0; i < 4; ++i) out[i] = i < count ? moved[i] : i == 3 ? 1.0f : 0.0f;
    return;
  }
  out[0] = moved[0];
  out[1] = count >= 2 ? moved[1] : 0.0f;
  out[2] = 0.0f;
  out[3] = stage->projected ? moved[count - 1] : 1.0f;
}

#endif
