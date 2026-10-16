
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

ShaderModuleDesc colored_vertex_shader();
ShaderModuleDesc colored_fragment_shader();
// Fills in the vertex layout of ColoredVertex (shaders left for the caller).
void describe_colored_pipeline(GraphicsPipelineDesc* desc);

}  // namespace graphics
