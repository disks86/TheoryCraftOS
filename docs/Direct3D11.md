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
| buffers | a Direct3D 12 buffer in an upload heap: `Map` hands its memory out, a draw uses its GPU address. A write while a recorded draw reads it gives the buffer other memory (see above). |
| constant buffers | Direct3D 12 here has one block of constants for all stages. The block of a draw is put together from the buffers bound to the vertex shader's slots, in slot order, then the pixel shader's, the geometry shader's, the hull shader's and the domain shader's, and goes into a ring of upload memory - so a constant buffer may change between draws without the list being sent. |
| textures | a Direct3D 12 texture in a default heap. The CPU reaches it through copies: `UpdateSubresource`, and `Map` of a dynamic or staging texture (a copy in memory, sent at `Unmap` or fetched at `Map`). R8 and A8 textures are kept as R8G8B8A8. |
| arrays, cubes and volumes | `CreateTexture2D` with `ArraySize` > 1 (up to 256) is the kernel's `Texture2DArray`; with `D3D11_RESOURCE_MISC_TEXTURECUBE` and six layers its `TextureCube` (sampled by direction; `d3d12.dll` is told with `TcosD3D12ResourceOptions` that the next resource is a cube, as Direct3D 12 knows a cube only by its views); `CreateTexture3D` (`ID3D11Texture3D`, depth up to 256) its `Texture3D`. Their subresources are the program's (`MipLevels` as asked for, each slice of a volume's level through `UpdateSubresource`'s box or `Map`'s depth pitch), but **the kernel keeps one level of them**: what is written to the others is dropped, and sampling at any level reads the first. They are sampled only. A shader resource view made without a description is a `TEXTURE2DARRAY`, `TEXTURECUBE` or `TEXTURE3D` view. |
| block-compressed textures | BC1, BC2 and BC3 (with their sRGB and typeless forms) are kept as they are: the rows of `UpdateSubresource`, `Map` and the initial data are rows of 4 x 4 blocks, and a box is on whole blocks. The kernel samples them as they are on the 3D hardware and decodes them for the CPU renderer ([Direct3DKernelMode.md](Direct3DKernelMode.md), *Block-compressed textures*). Sampled only; they are not copied with `CopyResource` / `CopySubresourceRegion`. |
| geometry, hull and domain shaders | go into the pipeline as they are (`gs_4_0` / `gs_5_0`, `hs_5_0`, `ds_5_0`: the kernel's, [Direct3DKernelMode.md](Direct3DKernelMode.md)); a draw with a hull shader needs a domain shader and a patch-list topology (`D3D11_PRIMITIVE_TOPOLOGY_n_CONTROL_POINT_PATCHLIST`). Their constant buffers come after the pixel shader's in the block (`TcosD3D12StageConstantBases`). |
| input layouts | the kernel takes 32-bit floats only: a layout with R8G8B8A8 or B8G8R8A8 colours gets its vertex buffers converted (a second buffer, made again when the first changes). |
| state objects | one Direct3D 12 pipeline state for each combination of shaders, layout, blend, depth-stencil and rasterizer state that is drawn with, in a cache of 96. |
| textures and samplers | a table of 8 of each (t0 .. t7, s0 .. s7) for all the stages: a slot the pixel shader has nothing bound to takes what the vertex, domain, geometry or hull shader has there (the kernel's texture units are the pipeline's, not a stage's). A slot with nothing bound samples a white texture of the kind the shaders declare there (2D, array, cube or volume: the kernel refuses a draw whose texture is of another kind). |
| swap chains | `dxgi.dll` asks the device for its back buffers and their pixels (`ITcosPresenter`, src/Windows/tcosdx.h); the device hands out the Direct3D 12 queue's back buffers inside its own textures. The device is an `IDXGIDevice` too (up to `IDXGIDevice3`: resources offered are never discarded, `EnqueueSetEvent` sets the event once the work before it ran). |
| queries | event, timestamp, timestamp-disjoint and occlusion queries. The queue runs each list before it returns, so `End` sends what was recorded and the query is answered: an event is done, a timestamp is `QueryPerformanceCounter` then. Occlusion is not counted (it says one sample passed). |

**The pixel shader's constants.** A Direct3D 11 pixel shader has constant buffers of its own, b0 .. b13, beside the vertex shader's. The
kernel compiles a shader's constant buffers end to end into the one block; for a pipeline made by `d3d11.dll` the pixel shader's begin where
the vertex shader's end. `d3d11.dll` reads the sizes out of the shaders (`dcl_constantbuffer`) and tells `d3d12.dll` the place with
`TcosD3D12PipelineOptions` (src/Windows/tcosdx.h) before it makes the pipeline; the kernel takes it with the shader
(`ShaderDesc::constant_base`, the escape `CreateShader`). All the constants of a draw are at most 4096 bytes.

**Depth clears.** Direct3D 12 here clears a depth buffer when the next pass with it begins, and forgets the clear with the list. A
`ClearDepthStencilView` is kept until a draw has followed it, and recorded again in every list until then.

## What is not there

- Deferred contexts and command lists; compute shaders (`CreateComputeShader`, `Dispatch`, unordered access views: the kernel and `d3d12.dll`
  have them, but buffers here are upload memory the GPU cannot write); `CreateGeometryShaderWithStreamOutput`; one stage's t<n> and another's
  of a different texture in one draw (they share the table).
- 1D textures and arrays of cubes; more than one level of an array, a cube or a volume (the others are dropped: the kernel keeps one); arrays,
  cubes and volumes as render targets (a dynamic cube map is refused); BC4 .. BC7; multisampling; mipmaps of a render target (`GenerateMips`
  does nothing); buffers as shader resources; unordered access; stream output; predicates, counters and pipeline-statistics queries
  (E_NOTIMPL / E_INVALIDARG); occlusion counts (an occlusion query says one sample passed).
- The sampler of t<n> is s<n>: the kernel pairs them by number. A shader that samples one texture with two samplers gets one.
- Blend factors (`D3D11_BLEND_BLEND_FACTOR` is 1), wireframe, depth bias, the back face's stencil operations, anisotropy, border colours,
  comparison samplers - what Direct3D 12 here has not.
- A draw needs a render target: there are no depth-only passes. All render targets of a draw have one size and format.
- Vertex formats other than 32-bit floats and the two 8-bit colour formats; an instance step rate other than 1.
- Textures above 4096 pixels a side; R8 and A8 as render targets.

A method that is not written answers E_NOTIMPL (or does nothing, for the ones that answer nothing), with the method's own signature - a
32-bit program's stack stays whole (scripts/gen-dx-vtables.py writes those).

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

`tests/windows/d3d11test.c` (100 checks): a device and its other faces, buffers, a triangle read back through a staging texture, a constant
buffer changed between two draws, the pixel shader's own constant buffer, colours as bytes in the vertices, a sampled texture and its
update, a dynamic texture, depth, blending, indexed draws, a scissor rectangle, the state read back, a cube sampled by direction on each
face (made with all its levels, a face updated, nothing bound), an array sampled by layer, a volume by slice (and one slice updated
through a box), a geometry shader that adds a copy of the triangle, tessellation with hull and domain shaders that read constant buffers
of their own, BC1 and BC3 textures (sampled, a block updated), a swap chain on a window. Run in the real kernel on its CPU renderer:
`tests/wow64_qemu.sh d3d11test` (32-bit) and `GPU=std tests/d3d12_qemu.sh d3d11test` (64-bit), and on the 3D hardware (virgl, the BC
textures as they are): `tests/d3d12_qemu.sh d3d11test`; all pass.
