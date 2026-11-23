# Direct3D 11

`d3d11.dll` (src/Windows/d3d11) is Direct3D 11 as a layer on this system's Direct3D 12 - what Windows calls "11 on 12". `d3d12.dll` is the
user-mode half of the kernel's Direct3D driver ([Direct3DKernelMode.md](Direct3DKernelMode.md)); `d3d11.dll` uses its public interfaces and
one private call. The same source is built for 64-bit and for 32-bit programs ([Wow64.md](Wow64.md)).

## How it works

A device is a Direct3D 12 device, one command queue and one command list. The immediate context only remembers what is bound; a draw works
out the Direct3D 12 pipeline of that state, sets on the list what changed since the last draw, and records the draw. The list is sent (the
queue runs it before it returns) when something has to be seen: `Flush`, a `Map` that reads, a `Present`, a query's `End`, a texture the list
drew into (or cleared, or copied into) that a draw samples, a texture the list sampled that becomes a target or is copied into. The kernel
keeps a texture bound the way a list first uses it, which is why those cases split the list (also a copy into or a clear of a texture the list
sampled, and a copy out of one it wrote).

**Writes to what a recorded draw reads** do not send the list. Sending it is a whole trip to the GPU and back, and Chrome (ANGLE, for Skia)
writes the buffer a recorded draw reads all the time: `Map` with `WRITE_DISCARD`, `UpdateSubresource`, buffer copies, and a different offset
into its streaming vertex buffer for every batch, which makes a new converted copy (see the input layouts below). Sending the list for each of
those took about 300 ms per composited frame. Instead the buffer gets other memory and the old is retired: kept, untouched, until the list
has run, then a spare for the next buffer of its size (`buf_rename`, `retire`, `take_upload` in d3d11.c). A converted copy has its own "used
by this list" mark (`converted_used`), apart from the buffer's, and a converted copy a recorded draw reads still sends the list when it is made
again. (Retiring it too made frames faster still, but on the 3D hardware Chrome's window then stopped updating now and then while typing into a
page: the GPU process went on drawing and presenting and the picture stayed. Not explained yet: the kernel took every submission, and the
virgl device is used one thread at a time (RenderNode's LockedDevice), so it is not a race on its object handles.) Texture uploads
(`UpdateSubresource`, `Unmap` of a dynamic texture) are recorded in order with the draws too. What waits for a list is capped
(`RETIRED_MAX`, 32 MiB); beyond that the list is sent first. In Chrome a page that repaints everything every frame went from 8 to about 30
frames a second.

**Buffers stay unmapped.** `d3d12.dll` keeps the CPU's copy of an upload buffer and sends it at `ExecuteCommandLists`. It compares every
*mapped* buffer with what it sent by a hash, byte by byte, at every submission, and sends one unmapped since then without comparing. So a
buffer here is mapped in Direct3D 12 only between the program's `Map` and `Unmap`; `Buf.data` stays valid all the time (d3d12.dll's `Map`
hands out the same memory every time), and a write the runtime does itself is marked with a `Map`/`Unmap` pair (`buf_touched`). The
constant ring is marked the same way when a list used it.

| Direct3D 11 | here |
|---|---|
| buffers | a Direct3D 12 buffer in an upload heap: `Map` hands its memory out, a draw uses its GPU address. A write while a recorded draw reads it gives the buffer other memory (see above). A buffer the GPU writes (`D3D11_BIND_UNORDERED_ACCESS`, `D3D11_BIND_STREAM_OUTPUT`) is in the GPU's memory (a default-heap buffer): `UpdateSubresource` and copies into it are copies the list makes, and what reads it on the CPU (a copy into a staging buffer, a vertex layout that converts it) sends the list and reads it back. `CreateBuffer` checks the rules of feature level 11_0 (usages, bind and misc flags, structured strides, raw views). |
| constant buffers | Direct3D 12 here has one block of constants for all stages. The block of a draw is put together from the buffers bound to the vertex shader's slots, in slot order, then the pixel shader's, the geometry shader's, the hull shader's and the domain shader's (a dispatch: the compute shader's), each as large as the shader declares it, and goes into a ring of upload memory - so a constant buffer may change between draws without the list being sent. Buffers are up to 64 KiB; `*SetConstantBuffers1` binds a window of one (first constant, number of constants). All the constants of a draw are at most 4096 bytes (the kernel's block). |
| textures | a Direct3D 12 texture in a default heap. The CPU reaches it through copies: `UpdateSubresource`, and `Map` of a dynamic or staging texture (a copy in memory, sent at `Unmap` or fetched at `Map`). Formats the kernel has no image of are kept in a wider one and converted texel by texel ([formats](#formats)). 1D textures (`CreateTexture1D`) are 2D textures a texel high: the kernel's shader translator takes `texture1d` / `texture1darray` as that. |
| arrays, cubes and volumes | `CreateTexture2D` with `ArraySize` > 1 (up to 256) is the kernel's `Texture2DArray`; with `D3D11_RESOURCE_MISC_TEXTURECUBE` and six layers its `TextureCube` (sampled by direction; `d3d12.dll` is told with `TcosD3D12ResourceOptions` that the next resource is a cube, as Direct3D 12 knows a cube only by its views); `CreateTexture3D` (`ID3D11Texture3D`, depth up to 256) its `Texture3D`. Their subresources are the program's (`MipLevels` as asked for, each slice of a volume's level through `UpdateSubresource`'s box or `Map`'s depth pitch), but **the kernel keeps one level of them**: what is written to the others is dropped, and sampling at any level reads the first. They are sampled only. A shader resource view made without a description is a `TEXTURE2DARRAY`, `TEXTURECUBE` or `TEXTURE3D` view. |
| block-compressed textures | BC1, BC2 and BC3 (with their sRGB and typeless forms) are kept as they are: the rows of `UpdateSubresource`, `Map` and the initial data are rows of 4 x 4 blocks, and a box is on whole blocks. The kernel samples them as they are on the 3D hardware and decodes them for the CPU renderer ([Direct3DKernelMode.md](Direct3DKernelMode.md), *Block-compressed textures*). Sampled only; they are not copied with `CopyResource` / `CopySubresourceRegion`. |
| geometry, hull and domain shaders | go into the pipeline as they are (`gs_4_0` / `gs_5_0`, `hs_5_0`, `ds_5_0`: the kernel's, [Direct3DKernelMode.md](Direct3DKernelMode.md)); a draw with a hull shader needs a domain shader and a patch-list topology (`D3D11_PRIMITIVE_TOPOLOGY_n_CONTROL_POINT_PATCHLIST`). Their constant buffers come after the pixel shader's in the block (`TcosD3D12StageConstantBases`). |
| input layouts | the kernel takes 32-bit floats only: a layout with R8G8B8A8 or B8G8R8A8 colours gets its vertex buffers converted (a second buffer, made again when the first changes). |
| state objects | one Direct3D 12 pipeline state for each combination of shaders, layout, blend, depth-stencil and rasterizer state that is drawn with, in a cache of 96. |
| textures and samplers | a table of 8 of each (t0 .. t7, s0 .. s7) for all the stages: a slot the pixel shader has nothing bound to takes what the vertex, domain, geometry or hull shader has there (the kernel's texture units are the pipeline's, not a stage's). A slot with nothing bound samples a white texture of the kind the shaders declare there (2D, array, cube or volume: the kernel refuses a draw whose texture is of another kind). |
| compute shaders | `CreateComputeShader`, `Dispatch`, `DispatchIndirect`: a Direct3D 12 compute pipeline of the shader (made at its first dispatch), the compute shader's constant buffers, shader resources (textures, raw and structured buffers) and samplers, and unordered access views u0 .. u7 in a table. The kernel binds u0 .. u3 and t0 .. t3 of buffers ([Direct3DKernelMode.md](Direct3DKernelMode.md)); a slot a shader declares with nothing bound reads a buffer of zeros (or writes one of its own). |
| unordered access views | of buffers: raw (`D3D11_BUFFER_UAV_FLAG_RAW`), structured, typed; an append or counter view has a hidden count, set with `CSSetUnorderedAccessViews` / `OMSetRenderTargetsAndUnorderedAccessViews` and copied with `CopyStructureCount`. The pixel shader's (and the other graphics stages') views share the slots with the render targets. `ClearUnorderedAccessViewUint` / `Float` fill a buffer's range or a texture's level and layers with the values in the view's format. Of textures the views are made, but the kernel has no unordered access to textures: a shader that declares one is refused. |
| buffers as shader resources | structured, raw (`BUFFEREX` with the raw flag) and typed views; in the shaders' table beside the textures. |
| hazards | as in Direct3D 11, a resource bound for writing (a render target, a depth buffer, an unordered access view, a stream output target) is taken out of the shader resource (and vertex and index) slots, and a shader resource view of what is bound for writing is left empty. |
| render targets of levels and layers | a render target with mip levels, or an array, a cube or a volume to draw into, is a texture the kernel samples, and each of its render target views has a target of its own the size of its level (rtshadow.inc): draws and clears go there, what views drew is copied into the texture before it is read, and a view's target is loaded from the texture when that was written another way. The copies go through a buffer and are recorded in order with the draws. A view of several layers draws into its first (`SV_RenderTargetArrayIndex` is not taken). `GenerateMips` filters on the CPU, in linear colour. |
| stream output | `CreateGeometryShaderWithStreamOutput` (also of a vertex or domain shader's code, which then captures that stage's outputs), `SOSetTargets` (an offset of -1 goes on from where the buffer's records end), `SOGetTargets`, `DrawAuto` (the vertices slot 0's buffer holds after its offset). Where the records end is read back after each list (the buffer `d3d12.dll` writes the filled size into). |
| deferred contexts | `CreateDeferredContext`: its setters keep its own bindings; what it is asked to do (draws, dispatches, clears, copies, updates, queries, the writes of a `Map` with `WRITE_DISCARD` / `WRITE_NO_OVERWRITE`) is recorded with a copy of the bindings it was asked under (deferred.inc). `FinishCommandList` hands the commands to an `ID3D11CommandList`; `ExecuteCommandList` runs them on the immediate context, each under its bindings, then restores or clears the immediate context's bindings; on a deferred context it records the list's commands. |
| Direct3D 11.1 | `ID3D11Device1` and `ID3D11DeviceContext1`: constant buffer windows, `ClearView` with rectangles, `Discard*` (the contents are left as they were: one of the things undefined contents may be), `CopySubresourceRegion1` / `UpdateSubresource1`, `CreateBlendState1` (no logic operations: `OutputMergerLogicOp` is FALSE), `CreateRasterizerState1`, `CreateDeviceContextState` and `SwapDeviceContextState`. The same description gives the same state object. The immediate context is an `ID3D11Multithread`; both contexts are `ID3DUserDefinedAnnotation` (no profiler: `BeginEvent` and `EndEvent` answer -1). |
| 11 on 12 and the Windows Runtime | `D3D11On12CreateDevice` makes a device on the program's Direct3D 12 device and queue; `ID3D11On12Device1` wraps its Direct3D 12 resources (`CreateWrappedResource`; a texture of a format the kernel keeps as it is). `D3D11CoreCreateDevice`. `CreateDirect3D11DeviceFromDXGIDevice` and `CreateDirect3D11SurfaceFromDXGISurface` give an `IDirect3DDevice` / `IDirect3DSurface` with `IClosable` and `IDirect3DDxgiInterfaceAccess`. |
| class linkage | `CreateClassLinkage`, `CreateClassInstance` and the instances bound with each stage's shader. The kernel has no dynamic linkage (interfaces, `fcall`): a shader with class instances is not run, and `GetClassInstance` finds none (`E_INVALIDARG`). |
| swap chains | `dxgi.dll` asks the device for its back buffers and their pixels (`ITcosPresenter`, src/Windows/tcosdx.h); the device hands out the Direct3D 12 queue's back buffers inside its own textures. The device is an `IDXGIDevice` too (up to `IDXGIDevice3`: resources offered are never discarded, `EnqueueSetEvent` sets the event once the work before it ran). |
| queries | event, timestamp, timestamp-disjoint, occlusion, occlusion predicate, pipeline statistics, stream output statistics and overflow predicates (all streams and each one). The queue runs each list before it returns, so `End` sends what was recorded and the query is answered. What the GPU knows the kernel measures for each draw (one query of the kernel's is open at a time, so a draw is measured for one kind: the samples first, then a stream's primitives needed, then the pixel shader's invocations; TcosD3D12BeginQueryKind). Pipeline statistics counted here: the input assembler's vertices and primitives, the vertex, hull, geometry and compute shaders' invocations, and without geometry or tessellation the primitives the rasterizer gets (C invocations and C primitives: clipping is not counted apart). Of a draw with tessellation the domain shader's invocations, and the primitives a tessellator or geometry shader puts out, are not known and not added; indirect draws are counted when a statistics query is active (their arguments are read back). Stream output's primitives written are the growth of what the targets hold. A timestamp is `QueryPerformanceCounter` at `End`. `SetPredication` leaves draws, dispatches, clears, copies, updates and resolves out while the predicate's result is the value given. The device has no counters of a driver's (`CheckCounterInfo`: none; `CreateCounter` answers `DXGI_ERROR_UNSUPPORTED`). |

**The pixel shader's constants.** A Direct3D 11 pixel shader has constant buffers of its own, b0 .. b13, beside the vertex shader's. The
kernel compiles a shader's constant buffers end to end into the one block; for a pipeline made by `d3d11.dll` the pixel shader's begin where
the vertex shader's end. `d3d11.dll` reads the sizes out of the shaders (`dcl_constantbuffer`) and tells `d3d12.dll` the place with
`TcosD3D12PipelineOptions` (src/Windows/tcosdx.h) before it makes the pipeline; the kernel takes it with the shader
(`ShaderDesc::constant_base`, the escape `CreateShader`). All the constants of a draw are at most 4096 bytes.

**Depth clears.** Direct3D 12 here clears a depth buffer when the next pass with it begins, and forgets the clear with the list. A
`ClearDepthStencilView` is kept until a draw has followed it, and recorded again in every list until then.

## Formats

`formats.inc` says how each texture format is kept. The kernel's images have four channels (RGBA32 float/uint/sint, RGBA16 float/unorm/snorm/uint/sint,
RGB10A2, RGBA8 unorm/sRGB/uint, BGRA8, the depth formats, BC1 - BC3). A format of fewer channels is kept in the four-channel format of its kind, with
8-bit snorm and sint channels in 16 bits; the channels it does not have read as 0 and alpha as 1, as Direct3D reads them, and a draw into one writes
only its own channels. R11G11B10_FLOAT, B5G6R5, B5G5R5A1, B4G4R4A4, R10G10B10A2_UINT and R9G9B9E5 are converted texel by texel. D16 is kept in 32-bit
float, D32_FLOAT_S8X24_UINT in D24S8 (with its stencil, 24 bits of depth). R24G8_TYPELESS and R32G8X24_TYPELESS not made for depth keep their bytes.
`CheckFormatSupport` answers from the table.

## What is not there

- Below `d3d11.dll`, in the kernel: arrays, cubes and volumes of formats wider than 32 bits a texel (RGBA16, RGBA32 and the formats kept in them -
  R32_FLOAT, R16G16_FLOAT ...), more than one level of an array, a cube or a volume (the others are dropped), arrays of cubes; depth buffers of
  several layers or levels, and sampling a depth buffer; unordered access to textures, typed buffer loads and stores (`Buffer<>`, `RWBuffer<>`),
  append and consume buffers (`imm_atomic_alloc` / `consume`), unordered access slots above u3 and buffer shader resources above t3; dynamic shader
  linkage; textures above 4096 a side; BC4 - BC7; `SV_RenderTargetArrayIndex` and `SV_ViewportArrayIndex` (several viewports: the first is drawn
  with); dual-source blend factors (`D3D11_BLEND_SRC1_*`: the pipeline is refused), the blend factor on GPUs other than virgl, wireframe, depth bias, the back face's stencil operations, anisotropy, border colours.
- The sampler of t<n> is s<n>: the kernel pairs them by number. A shader that samples one texture with two samplers gets one.
- One query of the kernel's is open at a time: a draw is measured for one kind (above).
- Shared resources: textures made `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` (with `SHARED` or `SHARED_KEYEDMUTEX`) are shared through NT handles
  (`IDXGIResource1::CreateSharedHandle`, `ID3D11Device1::OpenSharedResource1` / `OpenSharedResourceByName`, over d3d12.dll's shared resources). A texture made
  `SHARED_KEYEDMUTEX` has an `IDXGIKeyedMutex` (win32k's keyed mutex, shared with the texture's NT handle): `AcquireSync` (WAIT_TIMEOUT, WAIT_ABANDONED,
  DXGI_ERROR_INVALID_CALL when held already), `ReleaseSync` (sends what the device recorded first). Not there: the legacy handles of `IDXGIResource::GetSharedHandle` / `OpenSharedResource` (`DXGI_ERROR_UNSUPPORTED`, `E_INVALIDARG`), shared fences
  (`ID3D11Device5` is not made).
- A draw with no render target, no depth buffer and no stream output (unordered access only) is not made: the kernel's passes need a target.
- Vertex formats other than 32-bit floats and the two 8-bit colour formats; an instance step rate other than 1.
- The interfaces after 11.1 (`ID3D11Device2` .. `5`, `ID3D11DeviceContext2` .. `4`: tiled resources, fences, `Texture2D1` ...), the video interfaces
  (`ID3D11VideoDevice`, `ID3D11VideoContext`), `ID3D11InfoQueue` and `ID3D11Debug` (debug devices answer `DXGI_ERROR_SDK_COMPONENT_MISSING`, as
  Windows does without the SDK layers).
- The undocumented exports `D3D11CoreCreateLayeredDevice`, `D3D11CoreGetLayeredDeviceSize`, `D3D11CoreRegisterLayers`, `D3D11CreateDeviceForD3D12`,
  `D3DPerformance_*`, `EnableFeatureLevelUpgrade`, `OpenAdapter10` and `OpenAdapter10_2` (the Direct3D 10 driver interface) are still stubs.

## What it needed below it

- The kernel: a place for a shader's constant buffers in the block (above); a pipeline has the texture units its shaders read, not the
  number a root signature's table could hold (the CPU renderer takes no units for a pixel shader that samples nothing).
- `d3d12.dll`: the resources the descriptors of a table are views of go into the command stream's list of allocations (a draw that sampled
  a texture the same list had not also copied into was refused); a sampler's LOD limits are brought within what the kernel takes (`FLT_MAX`,
  the usual "no limit", was refused).
- The kernel does not carry two sets of values in the halves of one register between the vertex and the pixel shader (two `float2`
  outputs the compiler packs together): [Direct3D9.md](Direct3D9.md) found it and gives each a register. Not fixed.
- `dxgi.dll` no longer exports `D3D11CreateDevice`, `D3D12CreateDevice` and `D3D12GetDebugInterface` as stubs: a library linked against both
  got dxgi's.

## Finding out why nothing is drawn

A command stream the kernel's validator refuses is dropped whole and the program is not told. A kernel built with
`-DTCOS_TRACE_SERVICE_FAILURES=ON` says it on the debug console: "direct3d stream refused: command 10 after 11 commands, status ...
(Adapter.cpp line 536)" - the command is an `Op` of src/Kernel/Dxgkrnl/CommandStream.hpp, the line the check that refused a draw or a
binding (`dxgk::set_stream_trace`).

## Tests

`tests/windows/d3d11test.c` (188 checks): a device and its other faces, buffers, draws read back, constants a stage and their windows, colours as
bytes, textures of every kind and their updates, depth, blending, indexed and indirect draws, scissors, multisampling, the formats kept wider (cleared and
read back), 1D textures and arrays, compute (structured buffers in and out, atomics, UAV clears, `DispatchIndirect`, append counts), the pixel shader's
buffer views and UAVs, occlusion counts, predicates and predication, pipeline and stream output statistics, stream output with appending and `DrawAuto`,
`SetResourceMinLOD`, deferred contexts and command lists (a list in a list), the 11.1 methods, 11 on 12, the Windows Runtime's device and surface,
render targets of levels, layers and cube faces, `GenerateMips`, a swap chain on a window. The fast loop is the emulator with the kernel's renderer:

    tests/emulated_dx.sh tests/windows/d3d11test.c

and the real kernel under QEMU: `tests/wow64_qemu.sh d3d11test` (32-bit). Wine's conformance test (`tests/windows/wine/d3d11`, built with
`tests/windows/wine.py build <out> d3d11` and run with the emulator's `emurun`) gets through device creation, the contexts, deferred contexts and the
texture creation tests with 21 failures (16/32-bit layered textures and the video context, above; a child process the emulator does not start) and
stops at the creation of a depth buffer array, which the kernel cannot make.
