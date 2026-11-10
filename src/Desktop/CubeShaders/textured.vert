#version 450
// The logo cube of the 3D test (Vulkan path): position and texture coordinates in, the coordinates out.
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(push_constant) uniform Constants { mat4 mvp; } pc;
layout(location = 0) out vec2 v_uv;
void main() {
  gl_Position = pc.mvp * vec4(position, 1.0);
  v_uv = uv;
}
