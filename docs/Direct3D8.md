# Direct3D 8

`d3d8.dll` (src/Windows/d3d8) is Direct3D 8 as a layer on the COM interfaces of this system's Direct3D 9 (`d3d9.dll`,
[Direct3D9.md](Direct3D9.md)), the way the d3d8to9 project does it, written here from Direct3D 8's documentation and the behaviour Wine's
conformance tests record. It is built for 64-bit and for 32-bit programs ([Wow64.md](Wow64.md)); most Direct3D 8 programs are 32-bit.

## How it works

`Direct3DCreate8` makes an `IDirect3D9` and wraps it. Every Direct3D 8 object is a wrapper (`Object8` in d3d8.c) of the Direct3D 9 object that
does its work, and what is Direct3D 8's own is done in the wrapper. d3d8.h and d3d9.h cannot be in one file (they define the same enumerations and
structures with other members), so the Direct3D 9 interfaces are declared in `d3d9_abi.h`, which `scripts/gen-d3d9-abi.py` writes from mingw's
d3d9.h: the vtables method by method, and the structures whose names Direct3D 8 uses with another layout under names of their own
(`D3DPRESENT_PARAMETERS9`, `D3DSURFACE_DESC9`, `D3DVOLUME_DESC9`; d3d8.c checks their sizes for both widths). The vtables of the Direct3D 8
interfaces come from `scripts/gen-dx-vtables.py d3d8` (every method is written: none answers E_NOTIMPL).

| Direct3D 8 | here |
|---|---|
| identities | the device keeps a table from each Direct3D 9 object to its wrapper, so `GetTexture`, `GetRenderTarget`, `GetDepthStencilSurface`, `GetStreamSource`, `GetIndices`, `GetBackBuffer` hand back the object the program made. A texture keeps the surface (or volume) of each of its levels once it was asked for: `GetSurfaceLevel` gives the same object each time. If the program let go of a wrapper while Direct3D 9 still uses the object (a binding, a state block), a new wrapper is made for it. |
| references | a resource released to 0 and the device keep their emptied heads a while (64 resources a device, 8 devices), and answer a Release of an object with no references left with 0, as Direct3D 8 does (Wine's `test_refcount` releases a texture twice more and asks a device it no longer holds). Direct3D 8's rules, which Wine's `test_refcount` checks: a resource or swap chain holds its device; a level of a texture has the texture's references (`AddRef` on it is `AddRef` on the texture); the back buffers and the depth buffer made with the device or a swap chain have references of their own that start at 0 and hold the device while they have some, and go with the device; shaders and state blocks are handles that hold nothing. `SetTexture` and the other bindings take no reference of the program's wrapper. |
| `GetContainer` | the texture of a level; the device for every other surface. |
| private data | kept by the wrapper (Direct3D 8's `SetPrivateData` with `D3DSPD_IUNKNOWN`, `D3DERR_MOREDATA`, `D3DERR_NOTFOUND`): a level's is its own, not its texture's. |
| caps | `D3DCAPS8` of Direct3D 9's caps: the same numbers, the bits Direct3D 8 defines (Direct3D 9's later ones left out), `D3DPRASTERCAPS_ZBIAS` where there is a depth bias, `D3DPRASTERCAPS_ANTIALIASEDGES` where lines are smoothed, `D3DVTXPCAPS_NO_VSDT_UBYTE4` where `UBYTE4` cannot be read, `D3DCAPS2_CANRENDERWINDOWED`; shader versions of Direct3D 8: vertex shaders 1.1 and pixel shaders 1.4 at most. |
| adapters and modes | `GetAdapterModeCount` / `EnumAdapterModes` enumerate the modes of every display format in one list (X8R8G8B8, R5G6B5, X1R5G5B5, A2R10G10B10); `D3DENUM_NO_WHQL_LEVEL` is the opposite of Direct3D 9's `D3DENUM_WHQL_LEVEL`; `CheckDeviceType` takes no back buffer of `D3DFMT_UNKNOWN` (Direct3D 9 does, for a window). |
| `D3DPRESENT_PARAMETERS` | Direct3D 8's rules (a swap effect of 1 .. 4, at most 3 back buffers, one with the copy swap effects - which `Reset` does not check -, no refresh rate for a window), then Direct3D 9's form: `D3DSWAPEFFECT_COPY_VSYNC` is a copy at the vertical blank, a window presents at once with the other swap effects (Direct3D 9's default interval waits), `FullScreen_PresentationInterval` is the interval of a full screen, no multisample quality. What Direct3D 9 chose (the size of a window's back buffer, the count) is written back; of an additional swap chain only the count, as Direct3D 8 does. |
| `CreateDevice` | exactly one kind of vertex processing; a resource made with `D3DFMT_UNKNOWN` fails with the program's pointer as it was (an image surface's is set to NULL), as in Direct3D 8; the x87 control word is set to single precision, rounding to the nearest, every exception masked (0x7F) before the device is made, unless `D3DCREATE_FPU_PRESERVE`. |
| `Reset` | the implicit surfaces are the same objects with Direct3D 9's new surfaces in them; Direct3D 8's own state is a new device's (no shader set, no base vertex index); shaders and state blocks are kept. |
| image surfaces | `CreateImageSurface` is a surface in system memory (`D3DPOOL_SYSTEMMEM`). |
| `D3DSURFACE_DESC`, `D3DVOLUME_DESC` | with Direct3D 8's `Size`: the bytes of the level (blocks of 4 x 4 for DXT1 .. DXT5). |
| volumes | an `IDirect3DVolume8` is its volume texture's level: `LockBox`, `UnlockBox` and `GetDesc` are the texture's of that level (no `IDirect3DVolume9` is needed). |
| `SetRenderTarget(target, depth)` | a NULL target keeps the one that is set, a NULL depth buffer sets none; a depth buffer smaller than the target or of other multisampling is refused. |
| `CopyRects` | every rectangle (or the whole surface) to its point (or the same place), checked before anything is copied: the same format, no depth formats, within both surfaces, compressed formats by whole blocks, not a surface onto itself. Each is copied by what Direct3D 9 has for the two pools - `UpdateSurface` from memory into the device, `StretchRect` of one size between surfaces of the device, `GetRenderTargetData` of a whole target into memory - and by locking both surfaces where that is not the case or Direct3D 9 does not take it. |
| `GetFrontBuffer` | Direct3D 9's `GetFrontBufferData` of the device's swap chain. |
| streams and indices | `SetStreamSource(stream, buffer, stride)` is offset 0; a stream set to no buffer keeps its stride, as in Direct3D 8. `SetIndices(buffer, base vertex index)` keeps the base vertex index here, and `DrawIndexedPrimitive` passes it as Direct3D 9's `BaseVertexIndex`. `DrawIndexedPrimitiveUP` leaves no index buffer and with it no base vertex index. |
| render states | Direct3D 9's numbers, but: `D3DRS_ZBIAS` (0 .. 16, in units of the depth buffer) is Direct3D 9's `D3DRS_DEPTHBIAS` of -zbias units of the depth buffer that is set (worked out again when the depth buffer changes), `D3DRS_EDGEANTIALIAS` is `D3DRS_ANTIALIASEDLINEENABLE`, `D3DRS_SOFTWAREVERTEXPROCESSING` is `SetSoftwareVertexProcessing` (refused on a device of hardware vertex processing), `D3DRS_PATCHSEGMENTS` is `SetNPatchMode` (1.0 to begin with), and `D3DRS_LINEPATTERN` and `D3DRS_ZVISIBLE` are kept (Direct3D 8 itself draws with neither). A state Direct3D 8 has not (Direct3D 9's own, the holes of the list) is `D3DERR_INVALIDCALL`. |
| texture stage states | the ones that are sampler states in Direct3D 9 (`ADDRESSU/V/W`, `BORDERCOLOR`, `MAGFILTER`, `MINFILTER`, `MIPFILTER`, `MIPMAPLODBIAS`, `MAXMIPLEVEL`, `MAXANISOTROPY`) are `SetSamplerState` of the stage's sampler; stages 0 .. 7. |
| state blocks | `ApplyStateBlock(0)` does nothing and succeeds. A handle for a Direct3D 9 state block and what it holds of Direct3D 8's own state (the vertex and pixel shader handles, the base vertex index, the five render states above): all of it for `D3DSBT_ALL`, the pixel shader, line pattern and z bias for `D3DSBT_PIXELSTATE`, the vertex shader, software vertex processing and patch segments for `D3DSBT_VERTEXSTATE`, and what was set between `BeginStateBlock` and `EndStateBlock` (which then changes the block, not the device). While a block is recorded, `BeginStateBlock`, `CreateStateBlock`, `ApplyStateBlock` and `CaptureStateBlock` are refused. |
| draws | a draw of no primitives succeeds and does nothing (before anything is looked at). |
| `GetInfo` | the vertex cache, resource manager and vertex statistics (4 .. 6) are what the retail runtime of Direct3D 8 has no numbers of (`S_FALSE`); the texture manager and texturing ones (1 .. 3) were the debug runtime's (`E_FAIL`), other numbers are none. |
| the other methods | the same in Direct3D 9: clears, transforms, the viewport, material and lights, clip planes and clip status, the palette, cursor, gamma ramp, raster status, `ProcessVertices`, patches, `ResourceManagerDiscardBytes` (`EvictManagedResources`), lock flags, priorities, `UpdateTexture`. |

## Shaders

Shaders are handles. `CreateVertexShader(declaration, function, handle, usage)` reads the declaration's D3DVSD tokens into Direct3D 9 vertex
elements (`d3d8_shader.h`, which needs no library and which the host test includes as it is): `D3DVSD_STREAM`, `D3DVSD_REG` (a register is given
the usage Direct3D 8 gives it: position 0, blend weights, blend indices, normal 0, point size, colours 0 and 1, texture coordinates 0 .. 7,
position 1, normal 1), `D3DVSD_SKIP` (and `D3DVSD_SKIPBYTES` in the form of the Xbox's Direct3D 8), the tessellator's `D3DVSD_TESSNORMAL`
(`D3DDECLMETHOD_CROSSUV` of the position's element) and `D3DVSD_TESSUV` (`D3DDECLMETHOD_UV`), `D3DVSD_CONST` (the constants are loaded whenever
the shader is set), extensions (skipped), `D3DVSD_NOP` and `D3DVSD_END`; registers above 16, types above `SHORT4`, a register twice, a stream's
data in the tessellator's stream and offsets past 65535 bytes are refused. With no function the handle is the fixed-function pipeline with that
declaration (a normal of three floats only, as Direct3D 8 has it). A vs.1.0 / vs.1.1 has no dcl instructions - the declaration says what the
registers are -, so it becomes a Direct3D 9 vs_1_1 with a `dcl_<usage>` for each register the declaration fills, after the version token. The
shader is walked first (every instruction of vs 1.1 with its parameters - Shader Model 1 has no lengths -, registers of the kinds and numbers
vs 1.1 has, comments, the end token); as Windows does, a shader that reads an input register its declaration has not, or a constant beyond the
caps' `MaxVertexShaderConst` (by number or by `def`), is refused. `def` and Direct3D 9's own `dcl` are taken in a vs.1.1 (Windows takes the
shaders Wine's tests write with them); the program's dcl instructions are dropped for the declaration's. (`d3d8_shader.h` can also declare a
register no element fills, as binormal <register>, which Direct3D 9 reads as 0, 0, 0, 1; the device does not need it since it refuses such
shaders.) A handle above `0xF0000000` is a shader; one at or below it is an FVF code, which `SetVertexShader`
sets as Direct3D 9's FVF with no vertex shader. `GetVertexShaderDeclaration` and `GetVertexShaderFunction` give the program's own tokens
back with Direct3D 8's sizing (no buffer: the size; a buffer too small: `D3DERR_INVALIDCALL` with the size left as it was); the function of the
fixed-function pipeline is 0 bytes. Deleting the shader that is set leaves none set. `SetVertexShaderConstant` takes the constants of the caps
(256 with software vertex processing).

`CreatePixelShader` takes ps.1.0 .. ps.1.4 (each instruction known to its minor version: dp4 and cmp from 1.2, texm3x2depth in 1.3, texld /
texcrd / phase / texdepth / bem in 1.4 and the older texture instructions not there), ps.1.0 as Direct3D 9's ps_1_1; eight constants.

`ValidateVertexShader(code, declaration, caps, return_error, errors)` and `ValidatePixelShader(code, caps, return_error, errors)` check the same
rules: the version, every instruction and register, the end; with a declaration, that every input the shader reads is in it; with caps, the
version they allow and the constants they have (`MaxVertexShaderConst`). The text they write (with `return_error`; else an empty string) is a
string of the process heap; `ValidatePixelShader` of no code leaves `errors` as it is, as Windows does.

## The exports

`Direct3DCreate8`, `ValidateVertexShader`, `ValidatePixelShader`, `DebugSetMute` (the debug runtime's switch: the retail runtime has nothing to
mute) and `D3D8GetSWInfo` (the description of the software rasterizer's callbacks, which this Direct3D 8 has not: NULL), as in Wine's d3d8.spec.
The ordinals are the linker's (d3d8.def): no table of Windows' own was at hand to sync them with (`scripts/sync-ordinals.py`); programs take
these exports by name. mingw-w64 has no 64-bit `libd3d8.a`: the tests make one of `src/Windows/d3d8/d3d8.def` (`tests/windows/wine.py` for any
DLL of the OS mingw has no 64-bit library of, `tests/d3d12_qemu.sh` with dlltool).

## What is not there

What Direct3D 9 here has not, Direct3D 8 has not either, and fails as Direct3D 9 fails (E_NOTIMPL, or a draw without the effect):

- additional swap chains (`CreateAdditionalSwapChain`), more than one back buffer, `GetFrontBuffer` (`GetFrontBufferData`), `GetRasterStatus`,
  the palette (`SetPaletteEntries`, `SetCurrentTexturePalette` and the Get methods), `SetCursorProperties`, `ProcessVertices`, patches
  (`DrawRectPatch`, `DrawTriPatch`, `DeletePatch`), `GetClipPlane`, `SetClipStatus` / `GetClipStatus`: E_NOTIMPL in d3d9.dll.
- state block recording: Direct3D 9's `BeginStateBlock` records nothing apart (what is set between it and `EndStateBlock` changes the device
  at once, and the block is of everything). Direct3D 8's own state (shader handles, the base vertex index, its five render states) is recorded
  correctly here.
- `CopyRects`, `UpdateSurface` of parts and `StretchRect` of parts are not in Direct3D 9 here: the copy by locking stands in, which needs both
  surfaces lockable (not depth buffers).
- resource priorities (Direct3D 9's `SetPriority` keeps none), `GetNPatchMode` (0), the cursor, gamma ramps, the clip planes (`SetClipPlane`
  is taken and not drawn with): Direct3D 9's.
- the gaps of Direct3D 9's translator for the shaders of Direct3D 8 (Direct3D9.md): pixel shaders 1.x with `texbem`, `texbeml`, `bem`,
  `texm3x3tex`, `texm3x3spec`, `texm3x3vspec`, `texdepth` and `texm3x2depth` cannot be made; vertex shaders are drawn without the half pixel
  between Direct3D's pixel centres and the rasterizer's.
- `D3DRS_LINEPATTERN` and `D3DRS_ZVISIBLE` are kept and not drawn with (Direct3D 8 draws with neither).
- a depth buffer larger than the render target (which Direct3D 8 and 9 allow): with one set, nothing is drawn into a smaller texture in
  Direct3D 9 here (d3d8test draws into its texture with no depth buffer).
- a back buffer of 640 x 480 (the size Wine's visual tests take): after a draw the whole back buffer reads back as 0 in Direct3D 9 here,
  where 256 x 256 and smaller draw; most of Wine's visual.c fails on that, before anything of Direct3D 8 is looked at.
- the zero stride and NULL stream 0 Direct3D 8 leaves after `DrawPrimitiveUP`, `DrawIndexedPrimitive` with no index buffer set, the rules of
  `CheckDeviceFormat` for adapter formats, lockable back buffers (`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`), partial locks of compressed levels,
  the levels of a texture in one allocation, the transforms of vertex blending (`D3DTS_WORLDMATRIX(1 ..)`), multisampled targets: Direct3D 9's.

## Tests

`tests/D3d8ShaderTest.cpp` (host, ASan and UBSan): declarations of every register, type, stream, skip, tessellator output, constant and
extension as Direct3D 9 elements, and each malformed one refused; vertex shaders 1.0 / 1.1 rewritten with their dcl instructions and pixel
shaders 1.0 .. 1.4 as Direct3D 9 takes them, translated by d3d9.dll's own translator (`sm_to_dxbc.h`), which must take them and see each input
with its declaration's usage; the cases of Wine's `test_validate_vs` / `test_validate_ps`; every shortened declaration and shader, and 32000 with
bits turned.

`tests/windows/d3d8test.c` (77 checks): the Direct3D object, its modes and caps, presentation parameters Direct3D 8 refuses, references and identities (the
implicit surfaces at 0, a texture's levels, GetTexture / GetStreamSource / GetIndices, private data), draws read back by pixel (an FVF handle,
vertex and index buffers with SetIndices' base vertex index, a vertex shader of D3DVSD tokens with a constant its declaration loads, the
fixed-function pipeline through a declaration handle, pixel shaders 1.1 and 1.0, a texture through sampler states that are stage states),
Direct3D 8's own render states, state blocks by handle, CopyRects (rectangles to points, into and out of the back buffer), the sizes in the
descriptions, a volume level, a texture drawn into with `SetRenderTarget(target, depth)`, Present and Reset, ValidateVertexShader and
ValidatePixelShader. Run as a 32-bit program by `tests/wow64_qemu.sh d3d8test`, as a 64-bit one on the CPU renderer by
`GPU=std tests/d3d12_qemu.sh d3d8test` and on the 3D hardware by `tests/d3d12_qemu.sh d3d8test`: all 77 pass in each (2026-11-26). One check
reads a pixel back the way Wine's visual tests do (CopyRects of the render target into a level of a texture in system memory).

Wine's d3d8 tests (`tests/windows/wine/d3d8`, `scripts/winetest.sh d3d8`): see the table in tests/windows/wine/README.md. device.c stops at
the first test that uses what Direct3D 9 here has not (`test_swapchain`); to see every test function, a split build - one program per function,
made from device.c and visual.c into a folder of its own, run with `scripts/winetest.sh <folder>` and not checked in - was used: 69 of 145
functions pass, and what fails is Direct3D 9's (the list above).
