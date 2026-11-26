# Direct3D 9

`d3d9.dll` (src/Windows/d3d9) is Direct3D 9 and 9Ex - its fixed-function pipeline and a program's own vertex and pixel shaders - as a layer on this system's Direct3D 12 (`d3d12.dll`, the
user-mode half of the kernel's driver: [Direct3DKernelMode.md](Direct3DKernelMode.md)). It is built for 64-bit and for 32-bit programs
([Wow64.md](Wow64.md)). The caps say shader version 3.0 for both stages. `d3d8.dll` is a layer on its public interfaces.

## How it works

A device is a Direct3D 12 device, one command queue and one command list, as in `d3d11.dll` ([Direct3D11.md](Direct3D11.md)). What a
program hands over is kept in plain memory, and a draw brings it into the one form the shaders of this library take.

| Direct3D 9 | here |
|---|---|
| vertex and index buffers | plain memory; `Lock` hands it out. |
| a draw | each vertex it reads is decoded - whatever the layout, an FVF code or a declaration, and whatever the types - into a position, a colour, the fog factor it brings - the alpha of its second colour - a specular colour, and the coordinates of the texture stages the pixel shader reads (two, four or eight sets: 80, 112 or 176 bytes), in a ring of upload memory; the indices are copied into another ring, counted from the first vertex the draw reads. A triangle fan becomes a list. `DrawPrimitiveUP` and `DrawIndexedPrimitiveUP` take the same way from the program's memory. Every `D3DDECLTYPE` is decoded (`decode_element` in draw.inc): the floats, `D3DCOLOR` (blue first), `UBYTE4` and `SHORT2` / `SHORT4` as they are, the normalised ones (`UBYTE4N`, `SHORT2N` / `SHORT4N` and `USHORT2N` / `USHORT4N`; the most negative short is -1 as the next one is), `UDEC3` (three unsigned 10-bit fields as they are) and `DEC3N` (three signed ones over 511), `FLOAT16_2` / `FLOAT16_4`; what an element has not is 0, 0, 0, 1. |
| instancing | `SetStreamSourceFreq`: `D3DSTREAMSOURCE_INDEXEDDATA \| n` on stream 0 makes an indexed draw (`DrawIndexedPrimitive`) n instances (0: none), drawn one after the other with the vertices decoded for each; a stream of `D3DSTREAMSOURCE_INSTANCEDATA \| d` gives every vertex of instance i its element i / d (d = 0: element 0), the streams with the indexed-data flag or a plain divider are per vertex. A draw without indices makes one instance, the instance streams' first element. 0, both flags and instance data on stream 0 are `D3DERR_INVALIDCALL`; the value is kept as set (state blocks too). A plain divider other than 1 changes no draw: it is for software vertex processing of non-indexed data, which Direct3D 9's hardware devices do not do either. |
| the fixed-function pipeline | one vertex shader, written in HLSL and compiled ahead (below), and a pixel shader for each set-up of the texture stages a draw comes with (the texture stages, below). The vertex shader multiplies by world * view * projection, or takes positions that are pixels already (XYZRHW; Direct3D 9 has the pixels' centres at whole numbers, the rasterizer here at halves: half a pixel is added). The vertex shader also works out the fog factor where fog is by the vertex (`D3DRS_FOGVERTEXMODE`: linear, exponential, exponential squared, of the depth in view space - or, with `D3DRS_RANGEFOGENABLE`, of the distance from the eye), and passes on the factor the vertex brings where fog is by the vertex and there is no mode, or the positions are pixels. Positions that are pixels keep their fourth number, 1 / w: what is between the vertices is as the perspective has it, and fog by the pixel's depth can go by that w. The pixel shader does the texture stages, then the alpha test and fog as a program's pixel shader ends with them (below). |
| lighting | on the CPU, where the vertices are decoded (`ff_lighting.h`, which needs no library: the DLL has no C runtime). With `D3DRS_LIGHTING` a vertex's colour is the emissive colour, the ambient light on the ambient colour, and for each light that shines (the first 32 that are switched on - `MaxActiveLights` -, of lights of any number, kept in the order of their numbers) its diffuse light by the angle it falls at; with `D3DRS_SPECULARENABLE` a specular colour too, which the pixel shader adds after the stages. Directional lights, point lights (range, attenuation) and spotlights (the two cones and the falloff); the material, and each of its colours from the vertex instead where `D3DRS_COLORVERTEX` and the four material sources say so and the vertex has that colour; `D3DRS_AMBIENT`, `D3DRS_NORMALIZENORMALS`, `D3DRS_LOCALVIEWER`. Normals go through the inverse of world * view, transposed. Vertices that are pixels already, and a program's vertex shader, are not lit. Without lighting the specular colour is the vertex's second colour. |
| vertex blending | on the CPU too, where the vertices are decoded (`blend_vertex` in draw.inc): `D3DRS_VERTEXBLEND` of 0 to 3 weights blends a vertex's position and normal (through each matrix's inverse transpose) by the world matrices `D3DTS_WORLDMATRIX(0 ..)`, or with `D3DRS_INDEXEDVERTEXBLENDENABLE` the matrices its blend indices name (of 256), the last weight 1 less the others, into world space - the draw's world matrix is the identity then; `D3DVBF_TWEENING` blends the two positions and normals of a vertex by `D3DRS_TWEENFACTOR`. FVF codes convert to declarations as Windows does (`D3DFVF_XYZB5` has four weights and float indices; two weights with `D3DFVF_LASTBETA_D3DCOLOR` are a D3DCOLOR of weights and UBYTE4 indices). |
| texture coordinates | made on the CPU with the vertices (`ffl_texture_coordinates` in `ff_lighting.h`). A stage reads the set of the vertex's that `D3DTSS_TEXCOORDINDEX` names (of 8), or coordinates generated in view space: the normal, the position, the reflection vector, the sphere map. With `D3DTSS_TEXTURETRANSFORMFLAGS` they go through the stage's texture matrix - after the 1 Direct3D 9 puts behind the numbers a set has, so a matrix moves (u, v) by its third row - and the count says how many numbers come out. Projected coordinates are divided by the last of them in the pixel shader. Positions that are pixels already keep their sets as they are. |
| texture stages | eight. The states of the stages - operations, arguments, where a result goes - become a small key (`ff_stages.h`), and a key is written out as a pixel shader 2.0 that the translator below takes like a program's own, so it ends with the same alpha test and fog; the device keeps 48 of them by their keys. Every operation of Direct3D 9 for the colour and for the alpha but the bump environment ones (which pass on what they are given) and `D3DTOP_PREMODULATE` (the first argument); the arguments are the diffuse and specular colours, what the stage before gave, the stage's texture, the texture factor (`D3DRS_TEXTUREFACTOR`), the temporary register (`D3DTSS_RESULTARG`) and the stage's constant (`D3DTSS_CONSTANT`), each also as its complement or its alpha in every component; the third argument of `D3DTOP_MULTIPLYADD` and `D3DTOP_LERP`. A stage's result is clamped to 0 .. 1. The stages end at the first whose colour operation is `D3DTOP_DISABLE`; a stage with no texture that would read one passes on what it is given, the colour and the alpha each by itself. What an operation does not read is not in the key, so set-ups that draw the same share a shader. |
| render states | a Direct3D 12 pipeline for each combination of the shaders and the states a pipeline holds (culling, depth test and write, blending with separate alpha, the colour write mask, stencil), in a cache of 64. Pipelines of the same shader share the kernel's shader (`d3d12.dll` keeps each bytecode once): the kernel has 128 of a stage at a time where it runs them itself, and if a pipeline cannot be made the cache is emptied and it is tried again. The alpha test is a constant of the pixel shader; the scissor rectangle and the viewport are set with the target. |
| textures and surfaces | a Direct3D 12 texture of B8G8R8A8 whatever the program's format (A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8, R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, X4R4G4B4, A8R3G3B2, R3G3B2, A2R10G10B10, A2B10G10R10, G16R16, A8, L8, L16, A8L8, A4L4, P8, A8P8: formats.inc), or - the wide formats, whose pixels B8G8R8A8 would not keep - one of the
format's own pixels (A16B16G16R16, R16F, G16R16F, A16B16G16R16F, R32F, G32R32F, A32B32G32R32F, `wide_dxgi`), converted through floats; these are targets too, so a float target
keeps what is above 1. A sample has 1 in the channels its format has not, as Direct3D 9 has it (R32F is (r, 1, 1, 1)): the translator's fill registers, which a draw sets from
each sampler's texture (`format_fill`); the program's copy of a level is converted and sent when it is unlocked. Surfaces in system memory, scratch ones and off-screen plain surfaces of the default pool are memory only. Locking something the device owns (a target, a depth buffer of a lockable format, a shared texture) fetches its pixels, in the surface's format. Direct3D 9's rules of pools and usages: a texture of the default pool is locked only if it is dynamic, a target only if made lockable (`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` for the back buffer); no managed pool on 9Ex. The levels of a texture are objects of their own, made on the first ask (one `IDirect3DSurface9` a level and face, one `IDirect3DVolume9` a level of a volume texture: `GetVolumeLevel`, `LockBox`), which count with their texture. `D3DUSAGE_AUTOGENMIPMAP`: the program sees one level, the others are made from it (box or point filter) before a draw samples it after a change. `GetDC` / `ReleaseDC` of the 16-, 24- and 32-bit RGB formats: a DIB section of the surface's pixels, put back into it. Private data (`SetPrivateData`, an interface held with `D3DSPD_IUNKNOWN`), priorities and `SetLOD` of managed resources. |
| palettes | `SetPaletteEntries` (numbers below 65536, the entries' flags the alpha: `D3DPTEXTURECAPS_ALPHAPALETTE`), `SetCurrentTexturePalette`; P8 and A8P8 textures are sent through the palette that is current when a draw samples them (sent again when it changed). |
| cube and volume textures | `CreateCubeTexture` (`IDirect3DCubeTexture9`: faces by `LockRect` / `GetCubeMapSurface`, a face's surface locks the same pixels) and `CreateVolumeTexture` (`IDirect3DVolumeTexture9`: `LockBox`, slices one after the other; no `IDirect3DVolume9` objects) are the kernel's `TextureCube` and `Texture3D`, sampled by a program's pixel shader that declares `dcl_cube` / `dcl_volume`. They keep all the program's levels; the kernel keeps them too where it has mipmapped layers
(features2 bit 10: `D3DPTEXTURECAPS_MIPCUBEMAP`, `MIPVOLUMEMAP`), else only the first and the others are dropped. A cube made as a target has its faces (and the levels the
kernel keeps) as targets, each with a render target view of its own (`surface_target_view`); volumes are not targets. A sampler declared as a cube or a volume with nothing of that kind set samples a white one (and a 2D sampler with a cube set a white 2D texture): the kernel refuses a draw whose texture is of another kind. |
| compressed textures | DXT1 .. DXT5 are kept as their blocks (Direct3D 12's BC1, BC2 for DXT2 / DXT3, BC3 for DXT4 / DXT5; a locked level's pitch is a row of blocks): 2D textures and cubes, not volumes or targets. The kernel samples them as they are on the 3D hardware and decodes them for the CPU renderer. DXT2 and DXT4's premultiplied alpha is taken as it is. |
| render targets | the back buffer, textures and surfaces made as targets - any level of a target texture, any face of a cube target -; depth buffers of D24S8 (the 24-bit and stencil formats) or 32-bit float. A depth buffer larger than the target is used by its top left part, as in Direct3D 9 (the kernel takes a depth level as large as the pass or larger: the software, virgl and Venus devices draw so). Up to four targets at once where the kernel draws into several (`kFeatureMultipleTargets`: `NumSimultaneousRTs`), written by a pixel shader's `oC0` .. `oC3`, with the same blending and each its own `D3DRS_COLORWRITEENABLE1` .. `3` mask; `Clear` fills them all. Targets after a gap, or of another size than target 0, are not drawn into. A depth format is no colour target (`CreateRenderTarget(D3DFMT_D16)`: D3DERR_INVALIDCALL). |
| `ProcessVertices` | the fixed-function vertex stage on the CPU into the destination buffer, in the layout of the declaration given or of the buffer's FVF code: positions in pixels from the matrices and the viewport, lighting, vertex blending and tweening, the vertex fog factor in the specular alpha, texture coordinates as the vertex has them, the point size; with `D3DRS_CLIPPING` a vertex outside the clip volume is kept in clip space as Direct3D 9 does it ((x + w) / 2, (y + w) / 2, z, w) and the clip status gets the clip codes; `D3DPV_DONOTCOPYDATA` writes positions alone. With a program's vertex shader set it is refused (D3DERR_INVALIDCALL): there is no CPU interpreter of the shader models. |
| clears and copies | (blit.inc) `Clear` of the viewport, of rectangles in it (upside-down ones dropped) and of the scissor rectangle, each of colour, depth and stencil: Direct3D 12's clears for the whole of a target, else a rectangle drawn by the device itself with the device's state put aside. `ColorFill` of a target or of a plain surface of the default pool (a rectangle of it). `StretchRect` by Direct3D 9's rules of what goes where: a copy of one size is Direct3D 12's, one that scales or converts from what the device has is a rectangle of the device's own sampling the source (`D3DTEXF_POINT`, `D3DTEXF_LINEAR`), from memory the CPU's; depth buffers whole, outside a scene. `UpdateSurface` (a rectangle to a point), `UpdateTexture` (the dirty region of the top level - `AddDirtyRect`, locks without `D3DLOCK_NO_DIRTY_UPDATE` - and the lower levels; fewer levels in the destination are the source's smaller ones), `GetRenderTargetData`. |
| swap chains | (swapchain.inc) the device's implicit one and additional ones (`CreateAdditionalSwapChain`, `IDirect3DSwapChain9Ex`): up to 3 back buffers (30 on 9Ex) and a front buffer, the queue's textures. `Present` reads the back buffer to show and puts it into the window (`StretchDIBits`: a source rectangle into a destination rectangle of the window given or the swap chain's, through the dirty region for `D3DSWAPEFFECT_COPY`); then, for FLIP and DISCARD, the buffers move on (each back buffer has the next one's pixels, the last the old front buffer's: the surfaces stay, the textures behind them change places). The presentation interval waits for the vertical blank (`D3DKMTWaitForVerticalBlankEvent`). `GetFrontBufferData` (the desktop's size: the screen through GDI, the picture shown last where the window is), `GetRasterStatus` (`D3DKMTGetScanLine`, the kernel's 60 Hz model), `GetDisplayMode`, `GetPresentParameters`, the gamma ramp (kept, and applied to what a full-screen swap chain shows), the cursor (`SetCursorProperties` of an A8R8G8B8 surface of power-of-two sides becomes a cursor of the window system, `ShowCursor`, `SetCursorPosition`), `SetDialogBoxMode`'s conditions. Full screen: the display mode of the back buffers (`ChangeDisplaySettingsEx`, a mode of `EnumAdapterModes`, which are the display's), the window a top-most popup over the display, restored after. The swap chain's buffers and the device's depth buffer begin at no reference and keep the device while they have one, as Direct3D 9's do. |
| queries | (query.inc) `D3DQUERYTYPE_EVENT` (done when the list before it was sent), `OCCLUSION` (a Direct3D 12 occlusion query begun at the first draw after `Issue(D3DISSUE_BEGIN)`, ended when the device sends its list and begun again at the next draw, the lists' counts added up), `TIMESTAMP`, `TIMESTAMPFREQ` and `TIMESTAMPDISJOINT` (Direct3D 12 timestamps, nanoseconds), `VCACHE` (no vertex cache: size 0, strips); the others `D3DERR_NOTAVAILABLE`, as on Windows. `GetData` writes at most the size asked, `D3DGETDATA_FLUSH` sends what was recorded. |
| state blocks | `D3DSBT_ALL`, `D3DSBT_PIXELSTATE` and `D3DSBT_VERTEXSTATE` capture exactly Direct3D 9's sets of render, texture stage and sampler states, transforms, lights, constants, shaders, streams, textures (`StateMask`); between `BeginStateBlock` and `EndStateBlock` what is set goes into the block, not into the device, and the block applies and captures only that. |
| lost devices, `Reset` | a device that is not 9Ex is lost (`D3DERR_DEVICELOST`) while a full-screen one's window is not in front or is minimized, or when the display mode changed, and then `D3DERR_DEVICENOTRESET` until `Reset`, which fails while the program holds resources of the default pool (or the swap chain's buffers). `Reset` makes new buffers and the state of a new device; 9Ex keeps the state but for the viewport and the scissor rectangle. |
| 9Ex | `Direct3DCreate9Ex`, `CreateDeviceEx`, `ResetEx`, `PresentEx`, `GetDisplayModeEx`, `GetAdapterModeCountEx`, `EnumAdapterModesEx`, `GetAdapterDisplayModeEx` (with the display's rotation), `GetAdapterLUID`, `WaitForVBlank`, `CheckDeviceState` (occluded, mode changed), `Set/GetMaximumFrameLatency`, `Set/GetGPUThreadPriority`, `CheckResourceResidency`, `CreateRenderTargetEx` / `CreateOffscreenPlainSurfaceEx` / `CreateDepthStencilSurfaceEx`, `GetLastPresentCount` and `GetPresentStats` of the swap chain. Shared resources: `pSharedHandle` of a texture or surface of the default pool makes (pointing at NULL) or opens (pointing at a handle) a global share handle through `d3d12.dll`'s legacy shares (`TcosD3D12GlobalShare`, `TcosD3D12OpenShared`), the description carried with it and checked when opened (Direct3D 10/11/12 textures of B8G8R8A8 open too); a texture of system memory of one level, or a surface, may be the program's memory. |

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

The block of constants of a draw is the vertex shader's registers and then the pixel shader's, 1024 registers together at most, more than
any pair of shaders needs (the translator refuses a shader of more than 512): `SetVertexShaderConstantF` keeps 256 registers, `SetPixelShaderConstantF` 224, and a draw copies what its
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
samplers, `texldl` and `texldd`, textures in a vertex shader, a depth the pixel shader writes, `vPos` and `vFace`. The constants a version
has are checked (a pixel shader 1.x has c0 .. c7, 2.0 c0 .. c31, 3.0 c0 .. c223, a vertex shader c0 .. c255; bools and integers from
vs_2_0, ps_2_x and 3.0 on). The colour a vertex shader below 3.0 does not write (`oD0`) is white, and the half pixel between Direct3D 9's
pixel centres and the rasterizer's is added at the end of every vertex shader (a state register of `1 / W, -1 / H` times w).

**Memory.** The rings are small (1 MiB of vertices: 21845 a draw at most, fewer with a vertex shader of more than three inputs - 16 bytes an
input; 256 KiB of indices): every upload buffer is there three times -
in the program, in the kernel, on the device - and the kernel could not make rings of several megabytes resident beside each other.

## What is not there

- Of a program's shaders: what the translator refuses (above) - loops, subroutines, predicates, integer constants, `texldl` / `texldd`,
  vertex textures, the depth output, `vPos` / `vFace`, and of pixel shaders 1.x the bump environment and cube look-up instructions. The
  caps say 3.0 although not every 3.0 shader can be made.
- Software vertex processing keeps the hardware's limits: 256 float constants for vertex shaders, not the 8192 of Direct3D 9's software
  pipeline. `ProcessVertices` with a program's vertex shader (no CPU interpreter of the shader models).
- Of fog: a vertex without a second colour brings the fog factor 1 (no fog), a choice made here and not checked against Direct3D 9.
- Of texture coordinates: projection for vertices that are pixels already.
- Of the texture stages: bump environment mapping (`D3DTOP_BUMPENVMAP`, `D3DTOP_BUMPENVMAPLUMINANCE` and the signed formats they read),
  `D3DTOP_PREMODULATE`; cube maps of the fixed-function stages (their pixel shader samples 2D). The dot product writes the colour only,
  with the alpha by its own operation - not checked against Direct3D 9.
- Multisampling of any kind (`CheckDeviceMultiSampleType` says D3DERR_NOTAVAILABLE, a multisampled surface is D3DERR_INVALIDCALL), compressed
  volumes, volume targets, depth textures (INTZ, shadow maps) and cubes of depth.
- Several render targets with a gap between them, or of different sizes (Direct3D 12 here takes neither).
- `GetDC` of a 9Ex surface in the program's memory (the kernel has no `D3DKMTCreateDCFromMemory`); `ComposeRects` (no A1 surfaces).
- Of windows: the window procedure a full-screen device puts on its window (focus, minimizing, `WM_ACTIVATEAPP`) that Wine's `test_wndproc`
  checks.
- Patches (`DrawRectPatch`, `DrawTriPatch`) are refused, as on hardware whose caps have no `D3DDEVCAPS_RTPATCHES`.

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

`tests/windows/d3d9test.c` (320 checks, with `d3d9test_api.inc`): the Direct3D object and the adapter, a device on a window, clears, vertices that are pixels (to
the pixel) and vertices through the matrices, vertex and index buffers, culling, fans, textures of 32 and 16 bits, the stage operations,
the alpha test, blending, depth, the scissor rectangle, state blocks, the program's shaders (a vertex shader with the fixed-function pixel
stage, vertex and pixel shaders of 1.1, 2.0 and 3.0 from FVF vertices and from a vertex buffer through a declaration, constants of both
stages, a pixel shader that samples and discards, a pixel shader after the fixed-function vertex stage, pixel shaders 1.1 and 1.4, the alpha test and fog - by depth and by a vertex shader's factor - after a pixel shader), the fixed-function pipeline's alpha test
with each kind of function and its fog by the pixel's depth (by z, and by w under a perspective projection) and by the vertex (also by the distance: range fog; and by what vertices that are pixels bring - their w, and the factor in the second colour's alpha), drawing into a texture and sampling it, `GetRenderTargetData`,
`Present`, `Reset`, lighting (the lights kept and read back, ambient, a directional light by its angle, the colour sources, normals through the world matrix, a point light's distance and range, a spotlight's cone, the specular colour lit and unlit), texture coordinates (a texture matrix that moves and scales, projection, the set a stage reads, coordinates generated from the position), the texture stages (three and eight stages in a row, a stage with no texture and one switched off, every kind of argument, the operations that multiply, add, subtract, blend and take three arguments, the dot product, the temporary register, the alpha through the stages, the specular colour after them, and eighty set-ups one after the other - more than the device keeps shaders and pipelines for), and that everything is gone with its last reference. Also cube and volume textures (each face and slice
sampled by a program's pixel shader, a face filled through its surface, a cube sampler with nothing set) and DXT1, DXT3 and DXT5 textures.
`d3d9test_api.inc` adds the device's API beyond drawing, each part checking that the device's references are as they were after it: clears
of rectangles, ColorFill, StretchRect and UpdateSurface of parts and between sizes, UpdateTexture, GetDC, additional swap chains and their
buffers, the front buffer, gamma and the cursor, palettes, the levels of textures as objects, private data, the kinds of state blocks and
recording, every kind of query, the device's other methods, vertex blending and tweening, the wide formats (a float target keeps 2.0; R32F,
G16R16 and A2R10G10B10 sampled), levels and cube faces as targets, two targets at once with their own write masks, the write mask without
blending, `ProcessVertices` (pixels, clipping, the clip status, lighting, `D3DPV_DONOTCOPYDATA`), indexed draws with no index buffer, what the
UP draws leave of stream 0, a texture target smaller than the depth buffer, and 9Ex (devices, `ResetEx`, `PresentEx`, shared textures
between two devices, the Ex methods).
The fast loop is the host CPU emulator: `tests/emulated_dx.sh tests/windows/d3d9test.c` (the real kernel's Direct3D on its CPU renderer, in
a host process); Wine's d3d9 tests run the same way (tests/windows/wine/README.md).
Run in the real kernel on its CPU renderer: `tests/wow64_qemu.sh d3d9test` (32-bit) and `GPU=std tests/d3d12_qemu.sh d3d9test` (64-bit), and on
the 3D hardware: `tests/d3d12_qemu.sh d3d9test`. All pass on the CPU renderer; on virgl all but one of the fog checks (vertices that are
pixels with a 1 / w of 0, which should count as 1) - not looked into yet. `DxgkrnlTest` checks that the kernel compiles the
vertex shader.
