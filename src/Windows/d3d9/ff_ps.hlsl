// The pixel shader of d3d9.dll's fixed-function pipeline (src/Windows/d3d9/d3d9.c): compiled to shaders.inc by scripts/gen-d3d9-shaders.sh.
// Two texture stages. What a stage does is three weights for the colour and three for the alpha:
//   result = x * texture * current + y * texture + z * current
// (modulate: 1 0 0; the texture alone: 0 1 0; the stage switched off: 0 0 1; add: 0 1 1). A stage with no texture samples a white pixel.
// Then the alpha test and fog, by the same five registers as at the end of a program's pixel shader (sm_to_dxbc.h, SM_STATE_*).
cbuffer pixel_constants : register(b0) {
  float4 colour0, alpha0;   // the weights of stage 0
  float4 colour1, alpha1;   // ... and of stage 1
  float4 alpha_reference;   // x: the reference of the alpha test, 0 .. 255
  float4 alpha_keep;        // x, y, z: 1 where an alpha below, equal to, above the reference keeps the pixel, else 0
  float4 fog_colour;
  float4 fog;               // x, y: the linear factor is depth * x + y; z, w: the exponential one is 2 ^ -(depth * (z + w * depth))
  float4 fog_source;        // the factor is x * the vertex shader's + y * the linear + z * the exponential + w
};
Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);
float4 main(float4 position : SV_Position, float4 colour : COLOR0, float4 coordinate0 : TEXCOORD0, float4 coordinate1 : TEXCOORD1, float4 vertex_fog : TEXCOORD8) : SV_Target {
  float4 current = colour;
  float4 texel = texture0.Sample(sampler0, coordinate0.xy);
  current = float4(colour0.x * texel.rgb * current.rgb + colour0.y * texel.rgb + colour0.z * current.rgb, alpha0.x * texel.a * current.a + alpha0.y * texel.a + alpha0.z * current.a);
  texel = texture1.Sample(sampler1, coordinate1.xy);
  current = float4(colour1.x * texel.rgb * current.rgb + colour1.y * texel.rgb + colour1.z * current.rgb, alpha1.x * texel.a * current.a + alpha1.y * texel.a + alpha1.z * current.a);
  current = saturate(current);
  // the alpha as the eight bits Direct3D 9 compares
  float alpha = round(current.a * 255);
  float3 side = float3(alpha < alpha_reference.x ? 1.0 : 0.0, alpha == alpha_reference.x ? 1.0 : 0.0, alpha > alpha_reference.x ? 1.0 : 0.0);
  clip(dot(side, alpha_keep.xyz) - 0.5);
  float depth = position.z;
  float factor = dot(fog_source, float4(saturate(vertex_fog.x), saturate(depth * fog.x + fog.y), saturate(exp2(-(depth * (fog.z + fog.w * depth)))), 1));
  current.rgb = fog_colour.rgb + factor * (current.rgb - fog_colour.rgb);
  return current;
}
