# Direct3D 9

`d3d9.dll` (src/Windows/d3d9) is the fixed-function pipeline of Direct3D 9, as a layer on this system's Direct3D 12 (`d3d12.dll`, the
user-mode half of the kernel's driver: [Direct3DKernelMode.md](Direct3DKernelMode.md)). It is built for 64-bit and for 32-bit programs
([Wow64.md](Wow64.md)). A program's own vertex and pixel shaders are not there: the caps say shader version 0, and a program that asks
takes its fixed-function path.

## How it works

A device is a Direct3D 12 device, one command queue and one command list, as in `d3d11.dll` ([Direct3D11.md](Direct3D11.md)). What a
program hands over is kept in plain memory, and a draw brings it into the one form the two shaders of this library take.

| Direct3D 9 | here |
|---|---|
| vertex and index buffers | plain memory; `Lock` hands it out. |
| a draw | each vertex it reads is decoded - whatever the layout, an FVF code or a declaration, and whatever the types - into a position, a colour and two sets of texture coordinates (48 bytes), in a ring of upload memory; the indices are copied into another ring, counted from the first vertex the draw reads. A triangle fan becomes a list. `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` take the same way from the program's memory. |
| the fixed-function pipeline | one vertex shader and one pixel shader, written in HLSL and compiled ahead (below). The vertex shader multiplies by world * view * projection, or takes positions that are pixels already (XYZRHW; Direct3D 9 has the pixels' centres at whole numbers, the rasterizer here at halves: half a pixel is added). The pixel shader does two texture stages and the alpha test. |
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

**Memory.** The rings are small (1 MiB of vertices: 21845 a draw at most; 256 KiB of indices): every upload buffer is there three times -
in the program, in the kernel, on the device - and the kernel could not make rings of several megabytes resident beside each other.

## What is not there

- Vertex and pixel shaders of the program (`CreateVertexShader` and `CreatePixelShader` answer D3DERR_INVALIDCALL). The kernel's compiler
  for Direct3D 9 bytecode has 8 constant registers, two texture coordinates and no loops; a translation of shader models 1 to 3 into DXBC,
  which the kernel takes far more of, would be the way.
- Lighting: a vertex has its diffuse colour, or white. Materials and lights are taken and not used.
- Fog, specular, texture transforms and generated texture coordinates, the texture factor, more than two texture stages, the stage
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

`tests/windows/d3d9test.c` (54 checks): the Direct3D object and the adapter, a device on a window, clears, vertices that are pixels (to
the pixel) and vertices through the matrices, vertex and index buffers, culling, fans, textures of 32 and 16 bits, the stage operations,
the alpha test, blending, depth, the scissor rectangle, state blocks, drawing into a texture and sampling it, `GetRenderTargetData`,
`Present`, `Reset`, and that everything is gone with its last reference. Run in the real kernel on its CPU renderer:
`tests/wow64_qemu.sh d3d9test` (32-bit); the same source built for x86-64 passes too. `DxgkrnlTest` checks that the kernel compiles the
two shaders.
