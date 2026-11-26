# Direct3D 10

Direct3D 10 and 10.1 are three libraries, as on Windows: `d3d10core.dll` (the core: `D3D10CoreCreateDevice`), `d3d10.dll` (devices of feature level 10_0, state
blocks, effects, shader reflection and the other shader utilities) and `d3d10_1.dll` (devices of a feature level: `D3D10CreateDevice1`; everything else is
forwarded to `d3d10.dll`). As on Windows 7 and later, and as in Wine, the device is a Direct3D 11 device: `d3d11.dll` ([Direct3D11.md](Direct3D11.md)) makes it,
and every one of its objects that Direct3D 10 has a counterpart of is also that Direct3D 10 interface. The same source is built for 64-bit and for 32-bit
programs ([Wow64.md](Wow64.md)).

## How it works

**One object, two faces.** A Direct3D 11 object's head (`Child` in `src/Windows/d3d11/d3d11.c`) has one more pointer, `face10`: the table of its Direct3D 10
interface (`src/Windows/d3d11/d3d10.inc`). `QueryInterface` goes from the Direct3D 11 interface to the Direct3D 10 one and back; `IUnknown` is always the
Direct3D 11 one, the count is the object's, and so is its private data. The faces are `ID3D10Buffer`, `ID3D10Texture1D/2D/3D`, `ID3D10ShaderResourceView1`,
`ID3D10RenderTargetView`, `ID3D10DepthStencilView`, `ID3D10VertexShader`, `ID3D10GeometryShader`, `ID3D10PixelShader`, `ID3D10InputLayout`,
`ID3D10BlendState1`, `ID3D10DepthStencilState`, `ID3D10RasterizerState`, `ID3D10SamplerState` and `ID3D10Query` / `ID3D10Predicate` (one table: the predicate
adds no methods). What Direct3D 10 has no word for has no face, as on Windows: a structured buffer, a resource made with `RESOURCE_CLAMP`, tiles, a tile pool or
the protected flags, an extended (raw) buffer view, hull, domain and compute shaders, unordered access views, the per-stream queries. The textures' DXGI faces
(`IDXGISurface1`, `IDXGIResource`) are the same object, so `IDXGISwapChain::GetBuffer(IID_ID3D10Texture2D)` and `IDXGISurface` of a Direct3D 10 texture work
both ways.

**The device.** `TcosD3D10CoreCreateDevice` (a private export of `d3d11.dll`, which `d3d10core.dll`'s `D3D10CoreCreateDevice` calls) makes a Direct3D 11 device of
the one feature level asked for (10_0, 10_1, or 9_1 .. 9_3 for `D3D10CreateDevice1`) and gives it a second face, `ID3D10Device1` (`Dev.d10`). A device that
Direct3D 11 made answers no `ID3D10Device` (as on Windows), so the Direct3D 10 interfaces of its objects have no device (`GetDevice` gives NULL); every device
answers `ID3D10Multithread`, which is its immediate context's lock. A Direct3D 10 device is thread-safe unless it is made `SINGLETHREADED`.

Direct3D 10 has no device context: the device's methods are the immediate context's, with Direct3D 10's structures turned into Direct3D 11's - resource
descriptions (the misc flags `SHARED_KEYEDMUTEX` and `GDI_COMPATIBLE` have other numbers; bind flags Direct3D 10 has not are refused), viewports of whole
pixels, the depth-stencil view's description without `Flags`, the stream output declaration of one stream (one buffer with a stride, or one element in each of
up to four buffers with none; gaps of any number of components, which become Direct3D 11 gaps of at most four; every element checked against the shader's
output signature), Direct3D 10's blend (one blend for all targets, enabled target by target: Direct3D 11's with the blend copied into each target and
`IndependentBlendEnable` set where the targets differ), the pipeline statistics (Direct3D 10's eight counts), `D3D10_MAP` on the resources themselves
(`ID3D10Buffer::Map`, `ID3D10Texture2D::Map(subresource)` ... - only what has CPU access maps), `SOGetTargets` (an offset of ~0 for a bound buffer, as Windows
answers), the shader rules (shader model 4.0 code, 4.1 on a 10.1 device, of the stage asked for: Direct3D 9 code, shader model 5 and code of another stage are
`E_INVALIDARG`), `CheckFormatSupport` in Direct3D 10's bits (no unordered access, no gather below 10_1), `SetExceptionMode` / `GetExceptionMode`,
`SetTextFilterSize` / `GetTextFilterSize`, `OpenSharedResource` (any interface of the resource opened). Getters hand out the Direct3D 10 interfaces.

**References.** An object keeps a reference to its device only while the program has one to it (`Child.dev_held`): an object only the runtime holds - bound, or
viewed - does not keep the device, so a program that releases everything sees its device go with its last reference, as on Windows, though its state still held
objects (the device's end gives them back). This is d3d11.dll's rule for Direct3D 11 devices too.

**d3d10.dll** (`src/Windows/d3d10`): `D3D10CreateDevice` (`D3D10_SDK_VERSION` only; an adapter goes with the hardware driver type; WARP is the default adapter,
as this system's adapters are its own renderers; the reference rasterizer and the NULL device need the SDK's `d3d10ref.dll`; a software rasterizer is the
program's module for `IDXGIFactory::CreateSoftwareAdapter`), `D3D10CreateDeviceAndSwapChain` (the swap chain only when one is asked for), state blocks
(`stateblock.inc`: the `D3D10StateBlockMask*` functions with Windows' bit rules, and `ID3D10StateBlock` capturing and applying exactly the masked pieces through
the device's own getters and setters), the profiles (`D3D10Get*ShaderProfile`: shader model 4.1 on a 10.1 device, 4.0 otherwise and without a device,
`4_0_level_9_*` on a level 9 device, which has no geometry stage), `D3D10CreateBlob`, and:

- **Effects** (`effect.inc`, the parser `fx4.h`, the listing `fx4_disasm.h`): `D3D10CreateEffectFromMemory`, `D3D10CreateEffectPoolFromMemory` and every
  interface of `d3d10effect.h` over fx_4_0 and fx_4_1 binaries - techniques and passes (`Apply`, `ComputeStateBlockMask`, the pass and shader descriptions),
  every kind of variable with its get and set methods, arrays, members, elements, annotations, semantics, constant buffers (written into a Direct3D 10 buffer when
  a pass that uses them is applied after a change), shaders made on the device (also `ConstructGSWithSO`), state objects made from their descriptions, the
  effect-state expressions (index and value expressions: the preshaders' CLI4/CTAB/FXLC, evaluated), pools with their shared variables and child effects,
  `Optimize`. `d3d10_1.dll`'s effects are the same with Direct3D 10.1's blend (`TcosD3D10_1CreateEffect*FromMemory`, which its `.def` forwards to); `d3d10.dll`
  refuses an fx_4_1 effect as Windows' does. `D3D10DisassembleEffect` lists an effect in the shape of fxc's listing, its shaders through the disassembler below.
- **Shaders** (`shader.inc`, `dxbc_reflect.h`, `dxbc_disasm.h`, `hlsl_pp.h`): `D3D10ReflectShader` (`ID3D10ShaderReflection` and its constant buffers, variables
  and types, from the RDEF, signature and STAT parts; shader model 4 only, as Windows' d3d10.dll), the signature blobs (a DXBC container of those parts, with
  the container's checksum), `D3D10GetShaderDebugInfo` (the SDBG part), `D3D10DisassembleShader` (shader model 4 and 5 code, plain or HTML) and
  `D3D10PreprocessShader` (the C preprocessor of HLSL, with `ID3D10Include`).
- **Compiling HLSL** (`D3D10CompileShader`, `D3D10CompileEffectFromMemory`) is `d3dcompiler_47.dll`'s `D3DCompile`, as Windows' later d3d10.dll does. This
  system has no HLSL compiler: its `d3dcompiler_47.dll` answers `E_NOTIMPL` (it answered `S_OK` with no code before, which callers crashed on), so a program
  that compiles at run time needs Microsoft's `d3dcompiler_47.dll` next to it.

**dxgi.dll**: a factory of `CreateDXGIFactory` is DXGI 1.0's (`IDXGIFactory` alone; Direct3D 10 makes its factories so), `IDXGIFactory::CreateSwapChain`
refuses a description without a window (`DXGI_ERROR_INVALID_CALL`) or with a format no swap chain has (`E_INVALIDARG`), and a texture made `GDI_COMPATIBLE`
gives a device context (`IDXGISurface1::GetDC` / `ReleaseDC`: a memory DC with a DIB section of the texels, put back into the texture at `ReleaseDC`).

**d3d11.dll** also gained, for both APIs (Wine's tests reach them now that the Direct3D 10 interfaces are there): depth-stencil and shader resource view
descriptions checked and kept as Windows keeps them (the texture's format for UNKNOWN, the levels and slices that -1 stands for, the multisampled dimensions of
a texture of one sample, no typeless view formats), buffers with the render-target binding (their views are refused: below), raw buffers only for shader
resources and unordered access, `StructureByteStride` kept for structured buffers only, `IDXGIResource1` of buffers, a sampler's border colour kept only when it
addresses the border, `GetData` of a query being built (`DXGI_ERROR_INVALID_CALL`) and of no memory with a size (`E_INVALIDARG`), and `MinLOD` above `MaxLOD`
on a Direct3D 10 device.

## What is not there

- **An HLSL compiler**: see above. The Wine tests that compile at run time fail with `E_NOTIMPL` and then crash on the missing blob.
- **Render target views of buffers**: a buffer may be made with `D3D10_BIND_RENDER_TARGET`, but its render target view is `E_INVALIDARG` - the kernel draws
  into textures only (it would need a scratch texture copied into the buffer after each draw, as rtshadow.inc does for levels and layers).
- **`D3D10_FILTER_TEXT_1BIT`** (Direct3D 10's own filter for its text drawing): refused, the sampler below has no one-bit filter.
- **Rendering into array slices from a geometry shader** (`SV_RenderTargetArrayIndex`): the kernel's gap, [Direct3D11.md](Direct3D11.md).
- **Vertex buffer slots 8 .. 15**: bound and given back (Direct3D 10 has 16, Direct3D 11 32), but draws read slots 0 .. 7 (d3d11.dll's `VERTEX_SLOTS`, the
  kernel's vertex streams): a layout with an element in a later slot is not drawn.
- **Samplers s8 .. s15**: bound and given back (16 slots a stage, as Direct3D 10 and 11 have), but the kernel's sampler table has s0 .. s7 - a draw or dispatch
  whose shader declares a sampler above s7 is not made (said once on the debug channel). The same in Direct3D 11.
- **Shared buffers**: a buffer made `SHARED` has no shared handle (`GetSharedHandle` is `DXGI_ERROR_INVALID_CALL`); textures are shared ([Direct3D11.md](Direct3D11.md)).
- **Counters**: Direct3D 10's named counters are optional; this device has none (`DXGI_ERROR_UNSUPPORTED`).
- **The SDK layers** (the debug layer, `SWITCH_TO_REF`, `D3D10RegisterLayers`, `D3D10CoreRegisterLayers`): `DXGI_ERROR_SDK_COMPONENT_MISSING`, as Windows answers
  without them. `D3D10_RAISE_FLAG_DRIVER_INTERNAL_ERROR` is kept and answered; no driver error raises it.
- **Ordinals**: the exports have this build's order; no Windows d3d10 DLLs were at hand to take Windows' numbers from (scripts/sync-ordinals.py).
- **The listings** of `D3D10DisassembleShader` and `D3D10DisassembleEffect` follow fxc's layout but have not been compared with Microsoft's output line by line.
- The undocumented exports `D3D10GetVersion`, `D3D10CoreGetVersion` (the SDK version) and `RevertToOldImplementation` (`E_NOTIMPL`: there is no older runtime).

## Tests

- `tests/windows/d3d10test.c` (RESULTS_D3D10TEST): the device and its faces (10 <-> 11, DXGI, `ID3D10Multithread`, `D3D10CreateDevice1` at 10_1), clears and
  draws read back by pixel (a triangle, a constant buffer's matrix, indexed draws, a viewport), a sampled texture and its update, blend, depth and
  depth-stencil states, a geometry shader, stream output of a vertex shader's positions with its statistics query, occlusion, event, pipeline statistics and
  timestamp queries, predication, `Map` of buffers and textures, copies with a Direct3D 10 box, the 10 <-> 11 `QueryInterface` identity of a texture (one count,
  one IUnknown, `IDXGISurface`), private data through both faces, what has no Direct3D 10 face, state blocks, `ClearState`, a swap chain's `Present`, and a
  triangle drawn through an effect (variables set, `Apply`, a blended technique, `ComputeStateBlockMask`). Fast loop on the CPU renderer:

      tests/emulated_dx.sh tests/windows/d3d10test.c

  and on the real kernel: `GPU=std tests/d3d12_qemu.sh d3d10test` (CPU renderer), `tests/d3d12_qemu.sh d3d10test` (virgl), `tests/wow64_qemu.sh d3d10test` (32-bit).
- Host tests (ASan/UBSan): `tests/run_host_tests.sh Fx4ParserTest` (the fx_4 parser, expressions and listing over the fixtures of `tests/data/fx`, every
  truncation and every bit flip) and `tests/run_host_tests.sh DxbcReflectTest` (containers, checksums, reflection parsing, the disassembler and the preprocessor,
  truncations and bit flips).
- Wine's conformance tests (`tests/windows/wine/d3d10core`, `d3d10_1`, `d3d10`; RESULTS_WINE): `tests/windows/wine.py build <out> d3d10core d3d10_1 d3d10`, then
  each program with the emulator's `emurun`, or `scripts/winetest.sh d3d10core` under QEMU.
- The effect fixtures (`tests/data/fx/*.fx`, compiled `*.fxo`) are compiled with Microsoft's d3dcompiler_47 under Wine, as `scripts/hlsl-compile.sh` does for
  shaders (`TCOS_D3DCOMPILER`), with the profile fx_4_0 or fx_4_1 and no entry point.
