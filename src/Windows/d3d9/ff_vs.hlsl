// The vertex shader of d3d9.dll's fixed-function pipeline (src/Windows/d3d9/d3d9.c): compiled to shaders.inc by scripts/gen-d3d9-shaders.sh.
// Every vertex reaches it in one layout, whatever the program's was (d3d9.c converts): a position, a colour, two texture coordinates.
cbuffer vertex_constants : register(b0) {
  float4x4 transform;   // world * view * projection, for a row vector
  float4 screen;        // x, y: 2 / the viewport's width and height; z: 1 for positions that are pixels already (XYZRHW), else 0
  float4 offset;        // x, y: the viewport's left and top, in pixels
};
struct VertexIn { float4 position : POSITION; float4 colour : COLOR0; float2 texture0 : TEXCOORD0; float2 texture1 : TEXCOORD1; };
// (each set of coordinates has a register of its own between the stages: two sets in the halves of one register do not reach the pixel shader here)
struct VertexOut { float4 position : SV_Position; float4 colour : COLOR0; float4 texture0 : TEXCOORD0; float4 texture1 : TEXCOORD1; };
VertexOut main(VertexIn i) {
  VertexOut o;
  float4 transformed = mul(float4(i.position.xyz, 1), transform);
  // A pixel position: Direct3D 9 has the pixels' centres at whole numbers, the rasterizer here at halves.
  float4 pixels = float4((i.position.x - offset.x + 0.5) * screen.x - 1, 1 - (i.position.y - offset.y + 0.5) * screen.y, i.position.z, 1);
  o.position = lerp(transformed, pixels, screen.z);
  o.colour = i.colour;
  o.texture0 = float4(i.texture0, 0, 1);
  o.texture1 = float4(i.texture1, 0, 1);
  return o;
}
