
// Ready-made shaders that exist for both kinds of device (a native function for
// the software device, TGSI for the GPU device), so one program runs on either.
#pragma once
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"

namespace graphics {

// "Colored": a vertex is a position and a color; the shaders transform the
// position by the model-view-projection matrix passed as push constants
// (a Mat4: 64 bytes) and interpolate the color across the triangle.
struct ColoredVertex {
  float position[3];
  float color[4];
};

struct ColoredConstants {
  Mat4 mvp;
};

// The matrix of a fixed vertex kernel (the built-in coloured vertex shader: the 16 push-constant words are the columns of M) made from what a Direct3D 9 vertex shader takes in
// c0..c3: its rows when `rows` (transposed), and the sign of the Y row flipped when `flip_y` (Direct3D's +Y up to the device's +Y down).
inline void kernel_matrix_words(uint32_t mvp[16], bool rows, bool flip_y) {
  if (rows) {
    for (uint32_t r = 0; r < 4; ++r)
      for (uint32_t c = r + 1; c < 4; ++c) {
        const uint32_t t = mvp[4 * r + c];
        mvp[4 * r + c] = mvp[4 * c + r];
        mvp[4 * c + r] = t;
      }
  }
  if (flip_y) {
    for (uint32_t c = 0; c < 4; ++c) mvp[4 * c + 1] ^= 0x80000000u;
  }
}

ShaderModuleDesc colored_vertex_shader();
ShaderModuleDesc colored_fragment_shader();
// Fills in the vertex layout of ColoredVertex (shaders left for the caller).
void describe_colored_pipeline(GraphicsPipelineDesc* desc);

// The textured pair: the vertex shader transforms a float3 position (location 0) by the matrix in the push constants (the same first 64 bytes as the coloured one) and
// passes a float2 texture coordinate (location 1) on; the fragment shader writes the texel of texture unit 0 at that coordinate. The software, Intel Gen9 and virgl devices have them.
struct TexturedVertex {
  float position[3];
  float uv[2];
};
ShaderModuleDesc textured_vertex_shader();
ShaderModuleDesc textured_fragment_shader();
// The same, with every channel of the texel multiplied by the float that follows the matrix in the push constants (80 bytes in all): a picture drawn at an opacity.
// Only the software and virgl devices have it.
struct TexturedScaledConstants {
  Mat4 mvp;
  float scale;
  float pad[3];
};
ShaderModuleDesc textured_scaled_fragment_shader();
// The tinted textured pair, for glyphs: per vertex a position, a texture coordinate and a straight color (rgba, 0..1); the fragment shader writes that color with its alpha times the
// alpha of the texel of texture unit 0. The vertex shader takes the same matrix. Only the software and virgl devices have it.
struct TintedVertex {
  float position[3];
  float uv[2];
  float color[4];
};
ShaderModuleDesc tinted_vertex_shader();
ShaderModuleDesc tinted_fragment_shader();
void describe_tinted_pipeline(GraphicsPipelineDesc* desc);
// Filters of a picture of premultiplied colors, one render pass each (the vertex shader is the textured one; software and virgl only).
// Blur: one box blur pass along the step: the center texel and eight each way at multiples of the step (texture coordinates), with the nine mirrored weights.
struct TexturedBlurConstants {
  Mat4 mvp;
  float weight[12];   // 0 = center .. 8, the rest unused
  float step[2];
  float pad[2];
};
// A picture (texture 0, premultiplied) multiplied by the alpha of texture 1 at the same coordinate: a clip by a coverage mask. Its pipeline has two textures.
ShaderModuleDesc textured_masked_fragment_shader();
// The shadow of a picture: its alpha moved by `offset` (texture coordinates) and tinted with `color` (straight 0..1, alpha included), premultiplied.
struct TexturedShadowConstants {
  Mat4 mvp;
  float color[4];
  float offset[2];
  float pad[2];
};
ShaderModuleDesc textured_shadow_fragment_shader();
ShaderModuleDesc textured_blur_fragment_shader();
// Colour function on straight colors: rgb' = rows * rgb + add (each row: three factors and the add), clamped, alpha' = alpha * alpha_scale; the output is premultiplied.
struct TexturedColorConstants {
  Mat4 mvp;
  float row[3][4];
  float alpha_scale;
  float pad[3];
};
ShaderModuleDesc textured_color_fragment_shader();
// Fills in the vertex layout of TexturedVertex and the one texture (shaders left for the caller).
void describe_textured_pipeline(GraphicsPipelineDesc* desc);

}  // namespace graphics
