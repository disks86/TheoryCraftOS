# OpenGL

TheoryCraftOS has one OpenGL implementation, in the kernel (`src/OpenGl`). Every program personality reaches the same code: a user-mode library only encodes GL commands into
a stream and hands it over. Windows programs use `opengl32.dll` (`src/Windows/opengl32`); a Linux `libGL` would use the same client header and service.

```
program ── opengl32.dll ─────────── NtTcosOpenGl (509) ── GlService ── GlApi ── webgl::GlContext ── rasterizer + shader interpreter
           GlEntries.inc (generated)  src/Process/WindowsOpenGl.hpp     (contexts,   (desktop GL,   (src/WebGl: objects, draws,
           + WGL (opengl32.c)         src/OpenGl/WindowsGl.cpp          surfaces)    fixed function) GLSL ES front end)
```

## The pieces

| File | What it is |
| --- | --- |
| `src/OpenGl/khronos/gl.xml` | Khronos' registry (Apache 2.0), the source of every command signature and enum. |
| `scripts/gen-opengl.py` | Generates `src/OpenGl/generated/*`: the `GlApi` declarations, the stream decoder, opcodes and flags, enums, the client entry points and the `wglGetProcAddress` table, and the families of `glVertex3fv`-style commands. Run it after adding a `GlApi::gl*` method; `--report [-v]` prints coverage per GL version. |
| `src/OpenGl/GlStream.hpp` | The wire format: header word (opcode, length), arguments in words, fixed-size arrays inline. Commands with results or that read program memory are sent at once; the rest batch in the thread's buffer. |
| `src/OpenGl/GlClient.h` | The user-mode side (C): GL types and the command buffer the generated entries write. |
| `src/OpenGl/GlApi*.cpp` | One context. `GlApiCore` (decoding, display lists, client memory), `GlApiFixed` (matrices, lights, materials, fog, texture environments, enables), `GlApiImmediate` (`glBegin`/`glEnd`), `GlApiDraw` (client arrays, quads/polygons, draws), `GlApiBuffer` (buffers, mapping, VAOs, FBOs, per-buffer state), `GlApiTexture`, `GlApiPixels` (pixel transfer, `glDrawPixels`, `glBitmap`, `glCopyPixels`, accumulation), `GlApiProgram`, `GlApiGet`, `GlApiMisc` (raster position, evaluators, attribute stacks, selection/feedback, queries, sync). |
| `src/OpenGl/FixedFunction.cpp` | The fixed-function pipeline as generated GLSL ES programs, one per state key (lights, texture environments and combiners, texgen, fog, clip planes, two-sided lighting, point attenuation, colour sum), cached per context. |
| `src/OpenGl/GlslDesktop.cpp` | Desktop GLSL 1.10–3.30 to the GLSL ES dialects the front end compiles, compatibility built-ins (`gl_Vertex`, `gl_ModelViewProjectionMatrix`, `gl_TexCoord[]`, `ftransform()` ...) as declared variables the context fills. |
| `src/OpenGl/GlSurface.*` | A window or bitmap drawable: front/back colour, depth/stencil, accumulation. |
| `src/OpenGl/GlService.*` | All processes' contexts, share groups and surfaces; per-process lock; cleaned up at process exit. |
| `src/OpenGl/WindowsGl.cpp` | `NtTcosOpenGl`: the request layout is in `src/Process/WindowsOpenGl.hpp`. |
| `src/Windows/opengl32/opengl32.c` | WGL: contexts, `wglMakeCurrent` (windows and memory DCs), swaps and presenting, `wglShareLists`, `wglGetProcAddress`, `WGL_ARB_create_context(_profile)`, `WGL_ARB_pixel_format`, `WGL_ARB_extensions_string`, `WGL_ARB_make_current_read`, `WGL_EXT_swap_control`, font bitmaps. gdi32's `SwapBuffers` calls `wglSwapBuffers` when opengl32 is loaded. |

The engine (`src/WebGl/GlContext`) gained, for desktop GL only (WebGL behaviour is unchanged): shared object tables (rebindable for `wglShareLists`), default framebuffer images
owned by a surface, `DesktopState` (optional primitive restart, polygon modes with edge flags, line and polygon stipple, alpha test, logic op, per-draw-buffer blend and masks,
base vertex/instance, flat-shaded ranges, clip distances, depth clamp, a primitive sink for selection/feedback), 1D/rectangle/1D-array/cube-array/buffer/multisample texture targets,
more internal formats (luminance/intensity sizes, 16-bit, sRGB luminance), double/fixed vertex types, BGRA vertex colours and 32 attribute slots.

Presenting: a swap copies the front buffer (rows flipped) under the GL lock and then, outside it, puts it in the window through win32k when the window is a desktop frame; otherwise
opengl32.dll reads it back and draws it with `SetDIBitsToDevice` (also how memory-DC drawing reaches its bitmap). Single-buffered drawables show on `glFlush`/`glFinish`.

What it reports: `GL_VERSION` "3.3.0 Compatibility Profile", `GL_SHADING_LANGUAGE_VERSION` 3.30, core profiles up to 3.3 with `wglCreateContextAttribsARB`. Every extension in
`GL_EXTENSIONS` has all its commands (the generator checks; extensions it can only half cover lose their vendor-suffixed names so programs never find half an extension).

## Tests

- `tests/run_host_tests.sh OpenGlTest`: the generated client entries drive the real stream into `GlApi` (ASan+UBSan): immediate mode, quads/polygons, polygon modes, culling,
  alpha test, logic op, depth/blend, lighting, colour material, fog, textures and environments, client and buffer arrays, mapping, interleaved arrays, display lists (with copied
  client data and padded bitmaps), pixel paths and accumulation, attribute stacks, `glGet`, selection, feedback, ES and desktop GLSL programs, FBOs, evaluators, the core profile,
  share-group moves, the service, random and truncated streams.
- In the OS: `tests/windows/opengltest.c` (51 checks: pixel formats, contexts, fixed function, textures, lists, VBOs and shaders through `wglGetProcAddress`, font lists, shared
  contexts, a 3.3 core context, swaps) and Wine's `opengl32` test (`tests/windows/wine/opengl32`, 4111 checks, 299 failing when written).
  Build the program into `build/winetests/opengl32_own.exe` (`x86_64-w64-mingw32-gcc -O1 -o build/winetests/opengl32_own.exe tests/windows/opengltest.c -lopengl32 -lgdi32 -luser32`)
  and run `scripts/winetest.sh opengl32 own opengl` from a short path (a symlink to the checkout, with `build` a symlink to a build directory configured from it).

## The 3D hardware

When the machine has a GPU whose 3D engine takes TGSI (virgl in QEMU; `webgl::IAccelerator`, installed by `main.cpp` as `graphics::WebGlAccelerator` over the
system's graphics device), a context draws its window's default framebuffer there, the same way WebGL canvases do (`src/WebGl/GlAccel.cpp`, `TgsiTranslator.cpp`). Each
draw is offered to the device and runs on the CPU rasterizer when the device cannot do it; the choice is per draw and never changes the result, only where the work runs.

- The program's shaders become TGSI once (`translate_to_tgsi`, with the device's `AccelLimits`: virgl takes 256 constant rows, 16 attributes, 14 varyings and 16
  textures), including the generated fixed-function ones. The vertices are assembled on the CPU (the draw's modes, restart index and base vertex) and sent as one list.
- State the device has goes with the draw: blend factors and equations, the colour mask and the depth function (`AccelState::custom_blend`), culling, scissor and
  viewport, lines and points one pixel wide. What it has not (the stencil test, polygon offset, polygon modes and stipples, flat shading, alpha test, logic ops,
  wide lines, point sizes, two-sided colour, gl_PrimitiveID, clip planes, geometry and tessellation stages, queries, transform feedback, framebuffer objects,
  instancing, mipmapped or non-RGBA8 textures, constant blend colours ...) keeps the draw on the CPU.
- The picture stays on the device between draws. A clear of the whole window is done there only, and the CPU's depth and stencil clears wait until the CPU needs
  them (`hw_fast_clear`, `hw_apply_pending_clears`). The picture comes back to the window's image when something reads it: a swap or a front-buffer read
  (`GlService::sync_hardware`), `glReadPixels`, a draw the device cannot take. A swap keeps the device target for the next frame, which starts again at its clear.
- `GL_RENDERER` names the device. Each context counts its draws on the GPU and on the CPU, with why each refused one was refused (`GlContext::HardwareStats`); when
  a context goes, the kernel logs it ("OpenGL: process N: 1 draws on the GPU, 9 on the CPU; 5 because of ...").

Measured on virgl (QEMU, egl-headless): the 3D Test's GL cube went from 4.7 ms a frame on the CPU to 2.8 ms, of which 0.2 ms submitting and 2.6 ms bringing the
picture back. `tests/windows/opengltest.c` passes on virgl as on the CPU (`WINETEST_GPU=virgl scripts/winetest.sh opengl32 own`).

## The 3D Test window

The desktop's 3D Test can draw its cube through the kernel GL path (the command stream opengl32.dll writes, run by `GlService`), next to Direct3D; see [Direct3DKernelMode.md](Direct3DKernelMode.md#the-3d-test-window).

## Shader stages

- The GLSL front end (`src/WebGl/Glsl.cpp`) has a desktop mode (`GlslOptions::desktop`): GLSL 1.10-1.20 by the ES 1.00 rules, 1.30 and later by the 3.00 ones, plus what desktop
  GLSL adds. `GlslDesktop.cpp` only declares the compatibility built-ins (`tcos_*`), including the structure-valued uniform state that `GlApi` fills before each draw.
- Several shaders per stage link as one translation unit (`parse_glsl_units`), each preprocessed on its own; a function is visible only in the shaders declaring it.
- Geometry and tessellation (`GL_ARB_tessellation_shader`) stages: the front end has their layouts, built-ins (`gl_in[]`, `gl_out[]`, `gl_InvocationID`, `gl_TessCoord`, the levels),
  per-vertex arrays (and arrays of arrays inside them, flattened), `patch` varyings and `EmitVertex`/`EndPrimitive`/`barrier`. `GlStages.cpp` runs them between the vertex stage and
  the rasterizer: the control stage's invocations side by side to each `barrier()` with the patch's outputs in its shared region, the primitive generator of
  `Graphics/Graphics3D/Software/Tessellator.hpp` (the Direct3D 11 reference tessellator, also OpenGL's), then the evaluation and geometry stages. Adjacency primitives draw their
  inner primitive without a geometry stage.
- Compatibility varyings in those stages: the desktop translation renames `gl_FrontColor`, `gl_TexCoord[]`, ... to the `tcos_*` names the vertex and fragment stages use, and
  the front end gives `gl_in[]`/`gl_out[]` the matching members (only those a shader names). A compatibility program with such a stage and no fragment shader gets the
  fixed-function one. The colours the last stage writes are clamped to [0, 1] while `GL_CLAMP_VERTEX_COLOR` is on (`VaryingLink::clamp`; on the 3D device, a saturating move).
  Two-sided colour (`GL_VERTEX_PROGRAM_TWO_SIDE`): the back colours travel as extra vertex values past the interpolated ones (`ProgramObj::back_colors`), and a
  back-facing triangle's vertices take them in place of the front ones before it is rasterized. Fragment shaders are unchanged (such draws stay on the CPU).
- Transform feedback captures from the last vertex-processing stage. At link time the outputs become a plan: slot ranges, each at a byte offset of its buffer's per-vertex
  stride, with its vertex stream. The plan comes from the shader's own `xfb_buffer`/`xfb_offset`/`xfb_stride` layouts when it has them, else from the
  glTransformFeedbackVaryings names (`name`, `name[i]`, `Block.member`, `gl_SkipComponents1..4` leaving a gap, `gl_NextBuffer`). Each draw has a `GlContext::TfRecorder`: the stage that ends vertex processing hands it each vertex of a primitive, and the
  primitive is written whole or, when a buffer has no room, not at all. Strips go out as separate primitives, wound as they are drawn. The recorder also counts
  `GL_PRIMITIVES_GENERATED` and `GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN`, per stream (`glBeginQueryIndexed`). A geometry shader's `EmitStreamVertex(n)` on streams 1..3
  makes a point for those buffers and queries only (several streams need `points` output; only stream 0 is rasterized). Transform feedback objects keep their
  bindings, and they can be paused and drawn from (`glDrawTransformFeedbackStream`). WebGL 2 has the same objects, pause/resume and `getTransformFeedbackVarying`.
- `gl_PrimitiveID` in a fragment shader with no geometry stage: the draw counts its primitives per instance (`FragmentPipeline::next_primitive`), a quad drawn as two
  triangles counting once (`DesktopState::primitive_id_divisor`), a polygon as one, and a tessellated patch's primitives taking the patch's number.
- A `gl_PerVertex` redeclaration's array sizes (`float gl_ClipDistance[2];`) are what `.length()` says (`Global::declared_length`); the built-in keeps its slots.
- Layered rendering: `glFramebufferTexture` of a 3D, 2D array or cube texture attaches every layer (`Attachment::layered`; a cube's faces are layers 0..5), and the
  geometry stage's `gl_Layer` (the first vertex's, `GL_LAYER_PROVOKING_VERTEX`) picks the layer each primitive draws to; past the last layer it is the last. Clears reach
  every layer. Mixing layered and non-layered attachments is `GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS`.
- `GL_ARB_program_interface_query` (`GlApiResource.cpp`): uniforms with the block members after them, uniform blocks, vertex inputs, fragment outputs, transform
  feedback varyings (`ProgramObj::tf_resources`, also what `glGetTransformFeedbackVarying` reads: with xfb layouts, the captured outputs) and their buffers. Storage
  blocks, atomic counters and subroutines are valid interfaces with no resources. `glGetFragDataLocation` goes through the same code.
- piglit's `.shader_test` files: `tests/run_host_tests.sh PiglitShaderTest` (pinned piglit, fetched by `tests/fetch_piglit.sh`; tens of minutes), expected failures in
  `tests/data/piglit/shader_test.xfail`.

## Not done

- GLSL: doubles (so none in transform feedback either), compute shaders, subroutines, SSBOs/images, implicit conversions beyond prototypes; `gl_Layer` and
  `gl_ViewportIndex` read by a fragment shader (`GL_ARB_fragment_layer_viewport`), viewport arrays, layered 1D array textures; unnamed blocks' members captured by xfb
  layouts are reported as `Block.member`; `glBindFragDataLocation`.
- Clip distances (`gl_ClipDistance` with `GL_CLIP_DISTANCEi`, a compatibility shader's `gl_ClipVertex` against the user planes, or `gl_Position` when it writes neither) are
  tested per fragment, as interpolated varyings, like the fixed-function clip planes; a primitive is not cut geometrically.
- The 3D hardware takes only part of GL (see "The 3D hardware"): the translator has no loops, integers, user functions, uniform blocks, `discard` in a loop,
  flat varyings, `gl_FragCoord`, derivatives or texture kinds other than 2D; no framebuffer objects, stencil, mipmaps or instancing on the device; the picture is
  read back at each swap (no zero-copy present into the window). Only virgl takes TGSI today (the AMD GCN compiler takes a subset).
- No `KHR_debug` callbacks (a kernel cannot call into the program), no multisampling, `wglUseFontOutlines`, overlay planes, `wglCopyContext`, GL 4.x beyond the few 4.3/4.5
  queries programs call unconditionally, compressed images are not kept as blocks (`glGetCompressedTexImage` fails).
- Wine's test still fails on rendering into DIBs (bitmap formats' read-back of GDI contents), some pixel-format choice details, and object-name expectations after sharing.
