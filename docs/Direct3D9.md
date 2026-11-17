# Direct3D 9

`d3d9.dll` (src/Windows/d3d9) is Direct3D 9 - its fixed-function pipeline and a program's own vertex and pixel shaders - as a layer on this system's Direct3D 12 (`d3d12.dll`, the
user-mode half of the kernel's driver: [Direct3DKernelMode.md](Direct3DKernelMode.md)). It is built for 64-bit and for 32-bit programs
([Wow64.md](Wow64.md)). The caps say shader version 3.0 for both stages.

## How it works

A device is a Direct3D 12 device, one command queue and one command list, as in `d3d11.dll` ([Direct3D11.md](Direct3D11.md)). What a
program hands over is kept in plain memory, and a draw brings it into the one form the two shaders of this library take.

| Direct3D 9 | here |
|---|---|
| vertex and index buffers | plain memory; `Lock` hands it out. |
| a draw | each vertex it reads is decoded - whatever the layout, an FVF code or a declaration, and whatever the types - into a position, a colour and two sets of texture coordinates (48 bytes), in a ring of upload memory; the indices are copied into another ring, counted from the first vertex the draw reads. A triangle fan becomes a list. `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` take the same way from the program's memory. |
| the fixed-function pipeline | one vertex shader and one pixel shader, written in HLSL and compiled ahead (below). The vertex shader multiplies by world * view * projection, or takes positions that are pixels already (XYZRHW; Direct3D 9 has the pixels' centres at whole numbers, the rasterizer here at halves: half a pixel is added). The vertex shader also works out the fog factor where fog is by the vertex (`D3DRS_FOGVERTEXMODE`: linear, exponential, exponential squared, of the depth in view space - or, with `D3DRS_RANGEFOGENABLE`, of the distance from the eye). The pixel shader does two texture stages, then the alpha test and fog as a program's pixel shader ends with them (below). |
| texture stages | what a stage does is three weights for the colour and three for the alpha: `x * texture * current + y * texture + z * current` - modulate (also 2x and 4x), select either argument, add, off. A stage with no texture passes on what it is given. |
| render states | a Direct3D 12 pipeline for each combination of the states a pipeline holds (culling, depth test and write, blending with separate alpha, the colour write mask, stencil), in a cache of 64. The alpha test is a constant of the pixel shader; the scissor rectangle and the viewport are set with the target. |
| textures and surfaces | a Direct3D 12 texture of B8G8R8A8 whatever the program's format (A8R8G8B8, X8R8G8B8, R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8, L8, A8L8); the program's copy of a level is converted and sent when it is unlocked. A surface in system memory is memory only. Locking something that is drawn into fetches its pixels. |
| render targets | the back buffer, textures and surfaces made as targets; depth buffers of D24S8 (the 24-bit and stencil formats) or 32-bit float. `GetRenderTargetData` fetches a target into a surface in memory. |
| state blocks | a block holds all of the device's state, whatever kind was asked for. |
| `Present` | the back buffer's pixels are read and put into the window with `SetDIBitsToDevice`, as `dxgi.dll` does for its swap chains. |
| `Reset` | new targets of the new size, and the state of a new device. |

**The shaders.** `ff_vs.hlsl` and `ff_ps.hlsl` are compiled by `scripts/gen-d3d9-shaders.sh` into `shaders.inc`, which is checked in: the
build does not need the script. The script uses the `D3DCompile` of Wine's d3dcompiler_47 (vkd3d-shader), so it needs
`x86_64-w64-mingw32-gcc` and `wine`. The pixel shader's constants come after the vertex shader's in the one block of constants
(`TcosD3D12PipelineOptions`, [Direct3D11.md](Direct3D11.md)). Each set of texture coordinates has a register of its own between the two
stages: two sets in the halves of one register (what the compiler makes of two `float2`) did not reach the pixel shader through the
kernel's compiler - a gap there that this works around.

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
| samplers | two-dimensional textures `t<n>` with samplers `s<n>`. The tables of a draw hold 2, 4, 8 or 16, by the highest sampler the pixel shader reads (a root signature for each size, made when first needed). |
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

Refused by the translator: loops, `rep`, subroutines and the `break` instructions, the predicate register, integer constants, cube and
volume samplers, `texldl` and `texldd`, textures in a vertex shader, more than one render target, a depth the pixel shader writes, `vPos`
and `vFace`.

**Memory.** The rings are small (1 MiB of vertices: 21845 a draw at most, fewer with a vertex shader of more than three inputs - 16 bytes an
input; 256 KiB of indices): every upload buffer is there three times -
in the program, in the kernel, on the device - and the kernel could not make rings of several megabytes resident beside each other.

## What is not there

- Of a program's shaders: what the translator refuses (above). The half pixel between Direct3D 9's pixel centres and the rasterizer's
  is not made up for vertices that pass a vertex shader; a colour the vertex shader does not write reaches the pixel stage as 0, not as
  white. The caps say 3.0 although not every 3.0 shader can be made.
- Of fog: what vertices that are pixels already (XYZRHW) bring for it - the factor in the specular colour's alpha, and a w (their
  fog is by a table mode and by z only). Range fog is the fixed-function vertex stage's only, as in Direct3D 9.
- Lighting: a vertex has its diffuse colour, or white. Materials and lights are taken and not used.
- Specular, texture transforms and generated texture coordinates, the texture factor, more than two texture stages, the stage
  operations beyond the ones above, bump mapping.
- Cube and volume textures, compressed formats (DXT), 16-bit and floating-point targets, multisampling.
- Clears of a part of a target; `StretchRect` and `UpdateSurface` of a part or between sizes; `GetDC` of a surface.
- Clip planes, point size and sprites, wireframe, flat shading, depth bias, the blend factor, two-sided stencil, mirror and border
  addressing, anisotropic filtering - what Direct3D 12 here has not.
- Queries, additional swap chains, `IDirect3D9Ex`, exclusive full-screen modes (a device is its window's size or what it is asked for; one
  display mode is reported).
- The references are simpler than Direct3D 9's: a surface a program got from the device does not keep the device.

A method that is not written answers E_NOTIMPL with the method's own signature (scripts/gen-dx-vtables.py, which also checks that every
method that is written takes as many parameters as the interface gives it).

## Tests

`tests/SmToDxbcTest.cpp` (host, ASan and UBSan): shaders a compiler made (`tests/data/d3d9`, `scripts/gen-d3d9-test-shaders.sh`) are
translated, the kernel's compiler takes the result, and the program it makes gives the same numbers as the kernel's own compiler for
Direct3D 9 bytecode makes of the same shader; shaders written token by token give the numbers worked out for each kind of instruction
(constants through `a0`, bools, branches, `texkill`, the outputs of Shader Model 3, pixel shaders 1.x with their modifiers, factors
and texture registers, and the alpha test and fog at a pixel shader's end); what is refused; every shortened shader and 30000
with a bit turned.

`tests/windows/d3d9test.c` (101 checks): the Direct3D object and the adapter, a device on a window, clears, vertices that are pixels (to
the pixel) and vertices through the matrices, vertex and index buffers, culling, fans, textures of 32 and 16 bits, the stage operations,
the alpha test, blending, depth, the scissor rectangle, state blocks, the program's shaders (a vertex shader with the fixed-function pixel
stage, vertex and pixel shaders of 1.1, 2.0 and 3.0 from FVF vertices and from a vertex buffer through a declaration, constants of both
stages, a pixel shader that samples and discards, a pixel shader after the fixed-function vertex stage, pixel shaders 1.1 and 1.4, the alpha test and fog - by depth and by a vertex shader's factor - after a pixel shader), the fixed-function pipeline's alpha test
with each kind of function and its fog by the pixel's depth (by z, and by w under a perspective projection) and by the vertex (also by the distance: range fog), drawing into a texture and sampling it, `GetRenderTargetData`,
`Present`, `Reset`, and that everything is gone with its last reference. Run in the real kernel on its CPU renderer:
`tests/wow64_qemu.sh d3d9test` (32-bit); the same source built for x86-64 passes too. `DxgkrnlTest` checks that the kernel compiles the
two shaders.
