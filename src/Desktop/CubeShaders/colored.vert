#version 450
// The flat cube of the 3D test, drawn through Vulkan's path (SPIR-V translated to DXBC, then the kernel-mode Direct3D driver): position and face color in, the color out.
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(push_constant) uniform Constants { mat4 mvp; } pc;
layout(location = 0) out vec4 v_color;
void main() {
  gl_Position = pc.mvp * vec4(position, 1.0);
  v_color = color;
}
