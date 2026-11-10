#version 450
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D logo;
layout(location = 0) out vec4 out_color;
void main() { out_color = texture(logo, v_uv); }
