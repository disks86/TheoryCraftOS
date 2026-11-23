# Direct3D 9

`d3d9.dll` (src/Windows/d3d9) is Direct3D 9 - its fixed-function pipeline and a program's own vertex and pixel shaders - as a layer on this system's Direct3D 12 (`d3d12.dll`, the
user-mode half of the kernel's driver: [Direct3DKernelMode.md](Direct3DKernelMode.md)). It is built for 64-bit and for 32-bit programs
([Wow64.md](Wow64.md)). The caps say shader version 3.0 for both stages.

## How it works

A device is a Direct3D 12 device, one command queue and one command list, as in `d3d11.dll` ([Direct3D11.md](Direct3D11.md)). What a
program hands over is kept in plain memory, and a draw brings it into the one form the shaders of this library take.

| Direct3D 9 | here |
|---|---|
| vertex and index buffers | plain memory; `Lock` hands it out. |
| a draw | each vertex it reads is decoded - whatever the layout, an FVF code or a declaration, and whatever the types - into a position, a colour, the fog factor it brings - the alpha of its second colour - a specular colour, and the coordinates of the texture stages the pixel shader reads (two, four or eight sets: 80, 112 or 176 bytes), in a ring of upload memory; the indices are copied into another ring, counted from the first vertex the draw reads. A triangle fan becomes a list. `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` take the same way from the program's memory. |
| the fixed-function pipeline | one vertex shader, written in HLSL and compiled ahead (below), and a pixel shader for each set-up of the texture stages a draw comes with (the texture stages, below). The vertex shader multiplies by world * view * projection, or takes positions that are pixels already (XYZRHW; Direct3D 9 has the pixels' centres at whole numbers, the rasterizer here at halves: half a pixel is added). The vertex shader also works out the fog factor where fog is by the vertex (`D3DRS_FOGVERTEXMODE`: linear, exponential, exponential squared, of the depth in view space - or, with `D3DRS_RANGEFOGENABLE`, of the distance from the eye), and passes on the factor the vertex brings where fog is by the vertex and there is no mode, or the positions are pixels. Positions that are pixels keep their fourth number, 1 / w: what is between the vertices is as the perspective has it, and fog by the pixel's depth can go by that w. The pixel shader does the texture stages, then the alpha test and fog as a program's pixel shader ends with them (below). |
| lighting | on the CPU, where the vertices are decoded (`ff_lighting.h`, which needs no library: the DLL has no C runtime). With `D3DRS_LIGHTING` a vertex's colour is the emissive colour, the ambient light on the ambient colour, and for each light that shines (the first 8 that are switched on, of 32 numbers) its diffuse light by the angle it falls at; with `D3DRS_SPECULARENABLE` a specular colour too, which the pixel shader adds after the stages. Directional lights, point lights (range, attenuation) and spotlights (the two cones and the falloff); the material, and each of its colours from the vertex instead where `D3DRS_COLORVERTEX` and the four material sources say so and the vertex has that colour; `D3DRS_AMBIENT`, `D3DRS_NORMALIZENORMALS`, `D3DRS_LOCALVIEWER`. Normals go through the inverse of world * view, transposed. Vertices that are pixels already, and a program's vertex shader, are not lit. Without lighting the specular colour is the vertex's second colour. |
| texture coordinates | made on the CPU with the vertices (`ffl_texture_coordinates` in `ff_lighting.h`). A stage reads the set of the vertex's that `D3DTSS_TEXCOORDINDEX` names (of 8), or coordinates generated in view space: the normal, the position, the reflection vector, the sphere map. With `D3DTSS_TEXTURETRANSFORMFLAGS` they go through the stage's texture matrix - after the 1 Direct3D 9 puts behind the numbers a set has, so a matrix moves (u, v) by its third row - and the count says how many numbers come out. Projected coordinates are divided by the last of them in the pixel shader. Positions that are pixels already keep their sets as they are. |
| texture stages | eight. The states of the stages - operations, arguments, where a result goes - become a small key (`ff_stages.h`), and a key is written out as a pixel shader 2.0 that the translator below takes like a program's own, so it ends with the same alpha test and fog; the device keeps 48 of them by their keys. Every operation of Direct3D 9 for the colour and for the alpha but the bump environment ones (which pass on what they are given) and `D3DTOP_PREMODULATE` (the first argument); the arguments are the diffuse and specular colours, what the stage before gave, the stage's texture, the texture factor (`D3DRS_TEXTUREFACTOR`), the temporary register (`D3DTSS_RESULTARG`) and the stage's constant (`D3DTSS_CONSTANT`), each also as its complement or its alpha in every component; the third argument of `D3DTOP_MULTIPLYADD` and `D3DTOP_LERP`. A stage's result is clamped to 0 .. 1. The stages end at the first whose colour operation is `D3DTOP_DISABLE`; a stage with no texture that would read one passes on what it is given, the colour and the alpha each by itself. What an operation does not read is not in the key, so set-ups that draw the same share a shader. |
| render states | a Direct3D 12 pipeline for each combination of the shaders and the states a pipeline holds (culling, depth test and write, blending with separate alpha, the colour write mask, stencil), in a cache of 64. Pipelines of the same shader share the kernel's shader (`d3d12.dll` keeps each bytecode once): the kernel has 128 of a stage at a time where it runs them itself, and if a pipeline cannot be made the cache is emptied and it is tried again. The alpha test is a constant of the pixel shader; the scissor rectangle and the viewport are set with the target. |
| textures and surfaces | a Direct3D 12 texture of B8G8R8A8 whatever the program's format (A8R8G8B8, X8R8G8B8, R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8, L8, A8L8); the program's copy of a level is converted and sent when it is unlocked. A surface in system memory is memory only. Locking something that is drawn into fetches its pixels. |
| cube and volume textures | `CreateCubeTexture` (`IDirect3DCubeTexture9`: faces by `LockRect` / `GetCubeMapSurface`, a face's surface locks the same pixels) and `CreateVolumeTexture` (`IDirect3DVolumeTexture9`: `LockBox`, slices one after the other; no `IDirect3DVolume9` objects) are the kernel's `TextureCube` and `Texture3D`, sampled by a program's pixel shader that declares `dcl_cube` / `dcl_volume`. They keep all the program's levels, but the kernel has one: the others are dropped. Not render targets. A sampler declared as a cube or a volume with nothing of that kind set samples a white one (and a 2D sampler with a cube set a white 2D texture): the kernel refuses a draw whose texture is of another kind. |
| compressed textures | DXT1 .. DXT5 are kept as their blocks (Direct3D 12's BC1, BC2 for DXT2 / DXT3, BC3 for DXT4 / DXT5; a locked level's pitch is a row of blocks): 2D textures and cubes, not volumes or targets. The kernel samples them as they are on the 3D hardware and decodes them for the CPU renderer. DXT2 and DXT4's premultiplied alpha is taken as it is. |
| render targets | the back buffer, textures and surfaces made as targets; depth buffers of D24S8 (the 24-bit and stencil formats) or 32-bit float. `GetRenderTargetData` fetches a target into a surface in memory. |
| state blocks | a block holds all of the device's state, whatever kind was asked for. |
| `Present` | the back buffer's pixels are read and put into the window with `SetDIBitsToDevice`, as `dxgi.dll` does for its swap chains. |
| `Reset` | new targets of the new size, and the state of a new device. |

**The shaders.** `ff_vs.hlsl` is compiled by `scripts/gen-d3d9-shaders.sh` into `shaders.inc`, which is checked in: the build does not
need the script. The script uses the `D3DCompile` of Wine's d3dcompiler_47 (vkd3d-shader), so it needs `x86_64-w64-mingw32-gcc` and
`wine`. The pixel shader's constants come after the vertex shader's in the one block of constants (`TcosD3D12PipelineOptions`,
[Direct3D11.md](Direct3D11.md)): for a pixel shader of the texture stages they are the texture factor (c0), a register of numbers the
shader defines itself (c1) and the constants of the eight stages (c2 .. c9), then the state registers. Each set of texture coordinates
has a register of its own between the two stages: two sets in the halves of one register (what the compiler makes of two `float2`) did
not reach the pixel shader through the kernel's compiler - a gap there that this works around. The vertex shader always has the eight
sets as inputs; where the vertices of a draw have fewer, the others read the last that is there, and nothing reads them.

## A program's own shaders

`CreateVertexShader` and `CreatePixelShader` translate the shader's tokens into Shader Model 4 (a DXBC container) when the shader is made:
`sm_to_dxbc.h`, one header with no dependencies, which the host tests include as it is. That is the form the kernel's compiler takes most
of - 256 constant registers, integers, comparisons, branches - where its own compiler for Direct3D 9 bytecode has 8 constant registers
and two sets of texture coordinates. A shader the translator refuses cannot be made (D3DERR_INVALIDCALL).

| Direct3D 9 | in the translation |
|---|---|
| vs_1_1, vs_2_x, vs_3_0, ps_1_1 to ps_1_4, ps_2_x, ps_3_0 | taken. |
| float constants `c<n>` | the registers of `cb0` up to the highest one the shader names; a shader that addresses them through `a0` gets 224. Constants a shader defines itself (`def`) are written into its code. |
| bool constants `b<n>` | 16 registers after the floats, if the shader reads any; `defb` is decided when the shader is translated. |
| inputs of a vertex shader | one register of four floats for each `dcl`, named `ATTR<register>`. A draw finds the element of the vertex declaration (or of the FVF code) with the same usage and index and decodes it on the CPU, whatever its type; an input the vertices have not is 0, 0, 0, 1. |
| what goes from the vertex to the pixel shader | by what it is, which is how the kernel joins the stages: the two colours, texture coordinates 0 to 7, fog, and normal, tangent, binormal and blend weight of Shader Model 3 as further texture coordinates. So either stage can be the fixed-function one, which writes and reads colour 0 and texture coordinates 0 and 1. |
| samplers | textures `t<n>` with samplers `s<n>`: 2D, cube or volume as declared. The tables of a draw hold 2, 4, 8 or 16, by the highest sampler the pixel shader reads (a root signature for each size, made when first needed). |
| instructions | the arithmetic ones, the matrix products, `lit`, `dst`, `nrm`, `crs`, `sincos`, `pow`, `lrp`, `sgn`, `cmp`, `cnd`, `dp2add`, `if` on a bool and on a comparison, `texkill`, `texld` with its projected and biased forms, `dsx` and `dsy`. |

The block of constants of a draw is the vertex shader's registers and then the pixel shader's, 256 registers together at most (a draw whose
two shaders need more fails): `SetVertexShaderConstantF` keeps 256 registers, `SetPixelShaderConstantF` 224, and a draw copies what its
shaders read. A pipeline is cached for each pair of shaders and set of render states. Vertices whose positions are pixels already (XYZRHW)
pass no vertex shader, as in Direct3D 9.

**Pixel shaders 1.x** are another instruction set over the same tokens. Their `r<n>` are temporaries; their texture registers `t<n>`
(written, below 1.4) are temporaries of their own, filled by the instructions that sample: stage n is texture `t<n>` with sampler `s<n>`,
at the texture coordinates n. `tex`, `texcoord`, `texkill`, `texreg2ar`, `texreg2gb`, `texreg2rgb`, `texm3x2pad` with `texm3x2tex`,
`texm3x3pad` with `texm3x3`, `texdp3` and `texdp3tex` are there, and of 1.4 `texld`, `texcrd` and `phase`. A source's modifier (`_bias`,
`_bx2`, `1 - x`, `_x2`, `_dz`, `_dw`) is worked out into a temporary before the instruction; a destination's factor (`_x2`, `_x4`, `_d2`)
is a multiplication after it, and the saturation comes after that. The colour is what `r0` holds at the end. Refused: the bump
environment instructions (`texbem`, `texbeml`, `bem`: they read a matrix of the texture stage), the ones that look up a cube map
(`texm3x3tex`, `texm3x3spec`, `texm3x3vspec`) and the ones that write the depth (`texdepth`, `texm3x2depth`). Values are not kept
between -1 and 1 (or -8 and 8) as the hardware of the time did.

**After a pixel shader.** Direct3D 9 does the alpha test and fog on what a pixel shader gives, so every translated pixel shader ends with
them, steered by five registers of constants after the shader's own (`SM_STATE_*`), which a draw fills in from the render states: the
reference of the alpha test and which of below, equal and above keep the pixel (the alpha is compared as eight bits; all eight functions),
the fog's colour, and where the fog factor comes from - for shaders below Shader Model 3, which is where Direct3D 9 does fog: the depth
of the pixel when a table mode is set (linear, exponential, exponential squared; the shader reads its position for it), else the factor
the vertex stage gives - what a vertex shader of the program's writes (`oFog`), or what the fixed-function vertex shader works out. The
fixed-function pixel shader has the same five registers and the same ending. The depth is the pixel's z in the target, or - where the
projection matrix is a perspective one (its fourth column is not 0 0 0 1), as Direct3D 9 decides it - the w the pixel had before the
projection divided, which is the depth in view space: fog by w. A draw says which by two weights among the registers.

Samplers are declared as what the shader says (`dcl_2d`, `dcl_cube`, `dcl_volume`: `dcl_resource_texture2d / texturecube / texture3d`, `SmShader::sampler_kind`).
Refused by the translator: loops, `rep`, subroutines and the `break` instructions, the predicate register, integer constants, 1D
samplers, `texldl` and `texldd`, textures in a vertex shader, more than one render target, a depth the pixel shader writes, `vPos`
and `vFace`.

**Memory.** The rings are small (1 MiB of vertices: 21845 a draw at most, fewer with a vertex shader of more than three inputs - 16 bytes an
input; 256 KiB of indices): every upload buffer is there three times -
in the program, in the kernel, on the device - and the kernel could not make rings of several megabytes resident beside each other.

## What is not there

- Of a program's shaders: what the translator refuses (above). The half pixel between Direct3D 9's pixel centres and the rasterizer's
  is not made up for vertices that pass a vertex shader; a colour the vertex shader does not write reaches the pixel stage as 0, not as
  white. The caps say 3.0 although not every 3.0 shader can be made.
- Of fog: range fog is the fixed-function vertex stage's only, as in Direct3D 9. A vertex without a second colour brings the fog
  factor 1 (no fog), which is a choice made here and not checked against Direct3D 9 itself.
- Of lighting: more than 8 lights at once, light numbers above 31, vertex blending (the blend weights are skipped), and the fog factor
  lit vertices would get from the specular alpha is the vertex's own.
- Of texture coordinates: projection for vertices that are pixels already.
- Of the texture stages: bump environment mapping (`D3DTOP_BUMPENVMAP`, `D3DTOP_BUMPENVMAPLUMINANCE` and the signed formats they
  read), `D3DTOP_PREMODULATE`. The dot product writes the colour only, with the alpha by its own operation - not checked against
  Direct3D 9 itself.
- More than one level of a cube or volume texture (the kernel keeps the first), cube maps of the fixed-function stages (their pixel shader samples 2D),
  `texm3x3tex` and the other cube look-ups of pixel shaders 1.x, compressed volumes, 16-bit and floating-point targets, multisampling.
- Clears of a part of a target; `StretchRect` and `UpdateSurface` of a part or between sizes; `GetDC` of a surface.
- Clip planes, point size and sprites, wireframe, flat shading, depth bias, two-sided stencil, mirror and border
  addressing, anisotropic filtering - what Direct3D 12 here has not.
- Queries, additional swap chains, `IDirect3D9Ex`, exclusive full-screen modes (a device is its window's size or what it is asked for; one
  display mode is reported).
- The references are simpler than Direct3D 9's: a surface a program got from the device does not keep the device.

A method that is not written answers E_NOTIMPL with the method's own signature (scripts/gen-dx-vtables.py, which also checks that every
method that is written takes as many parameters as the interface gives it).

## Tests

`tests/SmToDxbcTest.cpp` (host, ASan and UBSan): shaders a compiler made (`tests/data/d3d9`, `scripts/gen-d3d9-test-shaders.sh`) are
translated, the kernel's compiler takes the result, and the program it makes gives the same numbers as the kernel's own compiler for
Direct3D 9 bytecode makes of the same shader (and a cube and a volume sampler are declared as the kernel's `TextureCube` and `Texture3D`); shaders written token by token give the numbers worked out for each kind of instruction
(constants through `a0`, bools, branches, `texkill`, the outputs of Shader Model 3, pixel shaders 1.x with their modifiers, factors
and texture registers, and the alpha test and fog at a pixel shader's end); what is refused; every shortened shader and 30000
with a bit turned.

`tests/FfLightingTest.cpp` (host, ASan and UBSan): the square root, cosine and power of `ff_lighting.h` against the host's, the matrix
for normals, each kind of light with numbers worked out by hand, 4000 random set-ups against the same sums written with the host's
functions, and 3000 set-ups with infinities and NaN in them, whose colours must still be 0 .. 1. The coordinates of a texture stage:
the matrix on one, two and three numbers, the counts, projection, each kind of generated coordinates, and numbers that are none.

`tests/FfStagesTest.cpp` (host, ASan and UBSan): the pixel shader of a set of stage states is translated, compiled by the kernel's
Direct3D and run, and compared with the stages worked out one after the other - the states a device starts with, every operation with
every kind of argument, eight stages, the temporary register, and 3000 set-ups picked at random; the key (stages end at the first that
is off, no texture passes on, what is not read is not in the key); keys of any bytes, buffers that are too small, and shaders shortened
or with a bit turned.

`tests/windows/d3d9test.c` (157 checks): the Direct3D object and the adapter, a device on a window, clears, vertices that are pixels (to
the pixel) and vertices through the matrices, vertex and index buffers, culling, fans, textures of 32 and 16 bits, the stage operations,
the alpha test, blending, depth, the scissor rectangle, state blocks, the program's shaders (a vertex shader with the fixed-function pixel
stage, vertex and pixel shaders of 1.1, 2.0 and 3.0 from FVF vertices and from a vertex buffer through a declaration, constants of both
stages, a pixel shader that samples and discards, a pixel shader after the fixed-function vertex stage, pixel shaders 1.1 and 1.4, the alpha test and fog - by depth and by a vertex shader's factor - after a pixel shader), the fixed-function pipeline's alpha test
with each kind of function and its fog by the pixel's depth (by z, and by w under a perspective projection) and by the vertex (also by the distance: range fog; and by what vertices that are pixels bring - their w, and the factor in the second colour's alpha), drawing into a texture and sampling it, `GetRenderTargetData`,
`Present`, `Reset`, lighting (the lights kept and read back, ambient, a directional light by its angle, the colour sources, normals through the world matrix, a point light's distance and range, a spotlight's cone, the specular colour lit and unlit), texture coordinates (a texture matrix that moves and scales, projection, the set a stage reads, coordinates generated from the position), the texture stages (three and eight stages in a row, a stage with no texture and one switched off, every kind of argument, the operations that multiply, add, subtract, blend and take three arguments, the dot product, the temporary register, the alpha through the stages, the specular colour after them, and eighty set-ups one after the other - more than the device keeps shaders and pipelines for), and that everything is gone with its last reference. Also cube and volume textures (each face and slice
sampled by a program's pixel shader, a face filled through its surface, a cube sampler with nothing set) and DXT1, DXT3 and DXT5 textures.
Run in the real kernel on its CPU renderer: `tests/wow64_qemu.sh d3d9test` (32-bit) and `GPU=std tests/d3d12_qemu.sh d3d9test` (64-bit), and on
the 3D hardware: `tests/d3d12_qemu.sh d3d9test`. All pass on the CPU renderer; on virgl all but one of the fog checks (vertices that are
pixels with a 1 / w of 0, which should count as 1) - not looked into yet. `DxgkrnlTest` checks that the kernel compiles the
vertex shader.
