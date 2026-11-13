# OpenGL

TheoryCraftOS has one OpenGL implementation, in the kernel (`src/OpenGl`). Every program personality reaches the same code: a user-mode library only encodes GL commands into
a stream and hands it over. Windows programs use `opengl32.dll` (`src/Windows/opengl32`); Linux programs use `libEGL.so.1` (`src/Linux/libegl`, see
[Linux: EGL and OpenGL ES](#linux-egl-and-opengl-es)), with the same client header, generated entry points and service.

```
program ── opengl32.dll ─────────── NtTcosOpenGl (509) ──┐
           GlEntries.inc (generated)  src/Process/WindowsOpenGl.hpp
           + WGL (opengl32.c)                            ├─ src/OpenGl/WindowsGl.cpp ── GlService ── GlApi ── webgl::GlContext ── rasterizer + shader interpreter
program ── libEGL.so.1 ──────────── /dev/dxg ioctl ──────┘                             (contexts,   (desktop GL,   (src/WebGl: objects, draws,
           GlEntries.inc + EGL        TCOS_DXG_IOCTL_GL                                  surfaces)    fixed function, GLSL ES front end)
           (src/Linux/libegl)         (src/Process/DxgDevice.h)                                       OpenGL ES)
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
- piglit's `.shader_test` files (`PiglitShaderTest`, fetched by `tests/fetch_piglit.sh`; run it from the checkout so it finds `tests/data/piglit/shader_test.xfail`).
  `--accel-survey` counts the draws the 3D hardware would take (the CPU still draws). `--accel-machine` draws on the reference device of `tests/TgsiAccel.hpp`:
  the translated TGSI run by `tests/TgsiMachine.hpp`, so a test that passes on the CPU and not there ("differs from the CPU") is a translation bug. It is slow
  (the machine reads the shader text for every pixel), so use `--filter`.

## Linux: EGL and OpenGL ES

`src/Linux/libegl/libegl.c` is EGL 1.5 for glibc programs, built by `src/Linux/libegl/build.sh` against the development machine's glibc and libwayland-client
(x86-64 only for now). It is installed in the Linux tree as `/usr/lib/libEGL.so.1`; `libGLESv2.so.2`, `libGL.so.1` and `libOpenGL.so.0` are empty libraries that
need it, so a program linked against Mesa's (glvnd's) libraries, or one that opens them with `dlopen` and looks commands up with `dlsym` (libepoxy: GTK), reaches
the one copy of the state in `libEGL.so.1`.

- **Transport.** The 64-byte request of `NtTcosOpenGl` (`src/Process/WindowsOpenGl.hpp`) goes to the kernel with `/dev/dxg`'s `TCOS_DXG_IOCTL_GL`
  (`src/Process/DxgDevice.h`); `opengl::gl_request` serves both. For a Linux process, memory handed to the program (strings, mapped buffers) comes from its mmap
  area (`linuxabi::map_user_memory`) and the kernel never shows a window: SWAP answers 0 and the library reads the picture itself. `DESTROY_SURFACE` (9) is new
  (eglDestroySurface; WGL never needed it).
- **OpenGL ES.** A context made with the ES flag (8) reports `OpenGL ES 3.0` / `OpenGL ES GLSL ES 3.00`, 3.0 for `GL_MAJOR_VERSION`/`GL_MINOR_VERSION` and a list of
  `GL_OES_*`/`GL_EXT_*` extensions; its shaders are always GLSL ES (one without `#version` is 1.00, not desktop 1.10). Otherwise it is the compatibility context, which
  allows what ES allows (client arrays with the default vertex array). ES 2.0 and 3.0 are given (a 3.0 context for both), not 1.x or 3.1+.
- **Desktop OpenGL** through EGL (`EGL_OPENGL_API`): up to 3.3, core (the default from 3.2, as EGL says) or compatibility.
- **Displays.** Wayland (`EGL_KHR_platform_wayland`, `EGL_EXT_platform_wayland`, and `eglGetDisplay` of a `wl_display`, recognised by its interface pointer as Mesa
  does) and surfaceless (`EGL_MESA_platform_surfaceless`, and `EGL_DEFAULT_DISPLAY`): pbuffers and contexts without a surface (`EGL_KHR_surfaceless_context`). Six
  configs: RGBA8 or RGB8 (`'AR24'` / `'XR24'` as the native visual), with depth 24 + stencil 8, depth 24, or neither, each for windows and pbuffers and every API.
  An `eglChooseConfig` that does not name a renderable type matches every config (EGL's default, ES 1.x, would match none).
- **Window surfaces** are `wl_egl_window`s (libwayland-egl): the library takes `driver_private` and the resize and destroy callbacks. `eglSwapBuffers` swaps the
  kernel's images, reads the new front image (top row first, B G R A) straight into a `wl_shm` buffer (`ARGB8888`, or `XRGB8888` for a config without alpha; one copy)
  and attaches, damages (`damage_buffer` from version 4; the rectangles of `EGL_KHR/EXT_swap_buffers_with_damage` flipped to the buffer's rows) and commits it. With
  a swap interval of 1 (the default) a swap first waits for the frame callback of the one before; 0 does not wait. Each window surface keeps up to three buffers and
  its own event queue and wrappers of the `wl_surface` and `wl_shm`, so the program's queues never see its events and two threads swapping two windows never share
  one. `wl_egl_window_resize` takes effect at once when the surface is current on the calling thread (the kernel's images are remade), else at the next swap; its
  `dx`/`dy` move the next buffer (`wl_surface.offset` from version 5).
- **Objects.** Handles are pointers checked against their display's lists, so a stale or made-up one is an EGL error. A context or surface destroyed while current
  goes when it is released (EGL's "marked for deletion"); `eglTerminate` marks everything. A context current on another thread is `EGL_BAD_ACCESS` (the kernel's
  `STATUS_RESOURCE_IN_USE`). Each thread's command buffer is allocated on first use (only a pointer is in thread-local storage, as a library opened with `dlopen`
  must keep it small) and goes, with what it had current, when the thread ends.
- **Fences** (EGL 1.5, `EGL_KHR_fence_sync`): every command has run when the call that sent it returns, so a fence is signalled when it is made.
- **Test.** `tests/linux_egl_wayland_emulated.sh` runs `tests/linux/eglwayland.c` (86 checks: strings, configs and their order, ES 2.0/3.0 yes and 1.x/3.1 no, a
  window surface on a decorated xdg-shell toplevel, GLSL ES 1.00 and 3.00 programs read back, swaps with interval 1 and 0 and with damage, a resize, a fence, a pbuffer
  with a shared context, a second thread refused the busy context and drawing with its own, `dlopen`/`dlsym` of `libGLESv2.so.2` and `libGL.so.1`, a desktop 3.3 core
  context, `eglGetProcAddress`) on the kernel's CPU emulator with the compositor and the kernel's GL (`-DTCOS_EMU_GL`); the window must reach the stand-in desktop with
  the last frame's colour. `tests/run_host_tests.sh OpenGlTest` covers ES contexts and surface destruction in the service. On the real kernel in QEMU (the program
  as the autorun, with `hold` as its argument to keep the window four seconds) it passed its 86 checks with standard VGA (the CPU renderer) and on virgl (its draws
  on the GPU), and the framed window showed the last frame. (It needed `clock_nanosleep`, Linux system call 230, which glibc's `nanosleep` makes: added.)
- **Not done.** No GLX (X11 programs), GBM, `EGL_KHR_image_base` / dmabuf import or export (`EGL_EXT_image_dma_buf_import`, `EGL_MESA_image_dma_buf_export`), buffer age,
  `EGL_EXT_present_opaque`, sRGB window surfaces, multisampled configs, OpenGL ES 1.x or 3.1+, a read surface other than the draw surface (the context reads where it
  draws), or binding pbuffers to textures. Frames go through `wl_shm` (one CPU copy into the buffer, and the compositor's), not a GPU buffer, even when the kernel's GL
  drew on the 3D hardware. Only x86-64 (the development machine has no other C library), and not yet tried with GTK or Qt inside the OS.

## The 3D hardware

When the machine has a GPU whose 3D engine takes TGSI (virgl in QEMU; `webgl::IAccelerator`, installed by `main.cpp` as `graphics::WebGlAccelerator` over the
system's graphics device), a context draws its window's default framebuffer there, the same way WebGL canvases do (`src/WebGl/GlAccel.cpp`, `TgsiTranslator.cpp`). Each
draw is offered to the device and runs on the CPU rasterizer when the device cannot do it; the choice is per draw and never changes the result, only where the work runs.

- The program's shaders become TGSI once (`translate_to_tgsi`, with the device's `AccelLimits`: virgl takes 256 constant rows, 16 attributes, 14 varyings and 16
  textures), including the generated fixed-function ones. The vertices are assembled on the CPU (the draw's modes, restart index and base vertex) and sent as one list.
- What the translator takes: loops (`for`, `while`, `do`, with break and continue), `switch`, user functions (inlined at each call, in / out / inout parameters,
  returns anywhere: a flag the statements after a return or continue are guarded by), integers and booleans (kept as float values, exact below 2^24; integer
  division truncates; shifts become multiplications), local arrays and uniform arrays indexed by variables (a chain of selects, and the address register),
  matrix and array varyings (a register per column), flat varyings (the last vertex, as virgl's rasterizer does; GL_FIRST_VERTEX_CONVENTION rotates the
  primitives), `gl_FragCoord`, `gl_FrontFacing`, `gl_FragData[0]`, discard, initialised globals, the trigonometric, hyperbolic, comparison and matrix built-ins
  (`inverse` and `determinant` of 2 x 2 and 3 x 3), texture lookups with a level, bias, gradients or projection (the device's textures have one level), and
  constants the decimal form cannot say (as their bits). Conditionals choose with CMP, so an infinity on the side not taken stays out of the result.
- Geometry and tessellation stages go to the device as stages of their own (`translate_program_to_tgsi`, `AccelStages`, `IAccelerator::create_program_stages`;
  the device's geometry, hull and domain modules). Each stage copies its per-vertex inputs (`IN[v][r]`, the position in register 0) into variables at its start, so
  they index like any array. Varyings between stages are GENERIC registers matched by name (`gl_in[].x` is the stage before's `x`, or the control stage's
  `gl_out[].x`), per-patch ones PATCH registers. The last stage before the rasterizer converts the position to the device's clip space. `EmitVertex()` writes the
  outputs and EMITs, `EndPrimitive()` is ENDPRIM. A control shader keeps its own control point's outputs in variables and writes them at the end, the control point
  chosen by a test of the invocation number for each (a register's vertex cannot be a variable). It writes its patch outputs and levels only if it wrote them,
  by a flag. `barrier()` puts all of that out first and then waits (TGSI BARRIER), so what an invocation reads of another's control point after it
  (`gl_out[j].x`, `x[j]`) is in that point's output register, chosen by `j`; a patch output it reads is its own value when it wrote one, else the register.
  Outputs the evaluation stage does not read get registers too, and other invocations' positions are read from a copy in a GENERIC register (virgl does not give
  back another invocation's POSITION output). When the evaluation shader reads the levels, they also go to it as two PATCH registers, because virgl cannot
  take TESSOUTER / TESSINNER as inputs. An evaluation stage without a control shader gets a generated one that passes the vertices on, with the default levels
  of `glPatchParameterfv` in two constant rows. The geometry stage's `gl_PrimitiveIDIn` is a PRIM_ID input; the others' `gl_PrimitiveID` and
  `gl_InvocationID` are system values. The draw's primitives are assembled on the CPU, as the CPU path assembles them (`GlAssemble.hpp`): patches, adjacency
  lists, and adjacency strips made lists. A program with a control stage is translated again when `GL_PATCH_VERTICES` changes. Arrays of arrays per vertex
  (`in vec4 v[][2]`, a block member array, `gl_in[].gl_ClipDistance`) put each vertex's elements in consecutive registers; the front end's flat index
  `i * inner + j` is decomposed again for a control stage's own point (`x[gl_InvocationID][j]`, `j` a variable too). `gl_in[].X` of any other built-in or
  compatibility varying (`gl_ClipVertex`, `gl_FrontColor`, `gl_TexCoord[]`: `tcos_*`) is the stage before's `X`. A per-vertex input holds only the vertices of
  the primitive or patch (a control stage's `gl_in[]` is `gl_MaxPatchVertices` long). Structures per vertex or per control point (`out S s;` into a geometry
  stage, `out S ts[]` of a control stage) take a register per column of each member, unpacked. Refused and left to the CPU: vertex streams; `gl_ViewportIndex`;
  flat varyings with `GL_FIRST_VERTEX_CONVENTION`.
- Varying packing: when a boundary's links take more registers than the device has, the links of scalars and vec2 share registers (four scalars or two vec2 to
  one, `IoLink::pack`, `spot_of`), written and read at their channels. Between the control and the evaluation stage the device has more room
  (`DeviceLimits::control_varyings`, 30 on a GL 4 virgl host). Structure varyings to the fragment shader take a register per column of each member.
- Fragment derivatives are DDX and DDY; TGSI's DDY runs down the window (the upper-left origin, as virgl showed), so `dFdy` is its negation. `gl_FragDepth` is
  the z of a POSITION output. `gl_VertexID` is an attribute the context fills with each vertex's number (`kTgsiVertexId`). Bitwise `&`, `|`, `^` go through the
  device's integers (F2I / F2U and back; exact while the values stay below 2^24, as all the translation's integers), `~` of a signed integer is `-x - 1`. A
  uniform array the shader uses whole (passed to a function, compared, copied) is copied into a variable at the start; the item lists that index by a variable
  are on the heap, so arrays of up to 128 elements are taken.
- Arrays of 32 plain values or more (not structures or samplers) are TGSI arrays: their temporaries are one range declared `DCL TEMP[a..b], ARRAY(n)`
  (`kIndexedArray`, `write_temps`), and an index the shader computes is clamped as the CPU clamps it and goes through the address register
  (`TEMP[ADDR[0].x+a](n)`, one MOV per column, also for `a[i].xz = ...`), so they may have up to 2048 elements. virgl makes each range an array of its own,
  so the other temporaries stay registers.
- Samplers are values the translation follows: a sampler parameter, a member of a uniform structure (also nested, also passed whole to a function), an
  element of a sampler array (by a constant index, as GLSL wants) are all one uniform slot, known when the shader is translated (`Val` kind 5, `Var::sampler`).
  Texture lookups work in every stage; the stages but the fragment one use `TXL` of level 0, and each stage declares the samplers it reads (the textures
  are bound to every stage the pipeline has). `textureSize(s, 0)` is the size row of the sampler's constant rows (another level's size is the CPU's).
- `continue` inside a `switch`: a flag of the switch, set before it leaves the switch, and after the switch a continue of the loop around. A conditional
  whose sides have effects (calls, assignments, `++`) runs only the side it takes (IF / ELSE), also with sides of no value (`b ? f() : g();`).
  `.length()` is the array's size. A fragment shader without a colour output draws with every channel masked (the CPU writes none). A varying that no
  stage before writes reads zero, as the CPU's vertices start (and an output that is declared but never written is written as zero, the flat link the
  CPU makes of it).
- `src/WebGl/AccelConformance.cpp` has 65 scenes that draw a picture with the accelerator and without, 22 of them desktop GL with geometry and tessellation
  stages (two invocations and `gl_PrimitiveIDIn`, adjacency, points made quads, triangles and quads at equal and odd spacing, patch varyings, an evaluation stage
  alone, tessellation and geometry together, outputs read across `barrier()`, clip distances and planes, no attributes, legacy shadow lookups, a rectangle texture,
  arrays per vertex and per control point, packed control-point arrays, `gl_ClipVertex` through `gl_in[]`, structures per vertex and per control point), plus a
  uniform block, shadow samplers, structures, derivatives, `gl_FragDepth`, `gl_VertexID`, bitwise operators, whole uniform arrays, structure varyings, packed
  varyings, samplers as parameters and members, TGSI arrays, a vertex-stage lookup with `textureSize`, `continue` in a switch and an unwritten varying in GLSL
  ES. The kernel runs them at boot on the machine's GPU ("OpenGL: ...: as the CPU renderer"; all 65 match on virgl), and
  `tests/GlAccelTest.cpp` runs them on a TGSI interpreter (`tests/TgsiMachine.hpp`, shared with DxgkrnlTest, with per-vertex registers and EMIT; the test's
  device runs the tessellator of `Software/Tessellator.hpp`). `PiglitShaderTest --accel-survey` counts which piglit draws the hardware would take and why the others not (`SURVEY_REASON=text` lists
  the tests): 1044 of 1223 tests draw only on the device (222 before the translator work, 709 before the geometry and tessellation stages, 755 before
  barriers, uniform blocks and clip planes, 825 before shadow samplers and structures, 923 before derivatives, arrays per vertex and packing, 1002 before
  samplers as values, TGSI arrays, lookups in every stage and the colour upload); most of the draws left are the variable-indexing stress tests of 31 vec3 /
  vec4 per vertex, more than any of virgl's stages take, then `gl_PrimitiveID` in a fragment shader, point sizes and shifts past 23 bits.
- Shadow samplers (`sampler1DShadow`, `sampler2DShadow`, `sampler2DRectShadow`; `texture`, `textureProj`, legacy `shadow1D` / `shadow2D` and their `Proj`
  forms): the depth texture goes to the device as a float texture with the depth in R (`IAccelerator::create_texture_float`, virgl's R32G32B32A32Float), read
  with nearest filtering, and the comparison is the shader's: three weights (reference below, equal to, above the depth) from the compare function, and with
  linear filtering four compared taps weighted as the CPU's percentage-closer filtering weighs them. `shadow1D` / `shadow2D`'s vec4 is `r * M + N` for
  `GL_DEPTH_TEXTURE_MODE`. These come from four constant rows of the sampler (`TgsiSampler::params`, filled at each draw from the texture and sampler state), so
  one translation serves every state. 1D textures are w x 1 device textures read at t = 0.5; rectangle textures divide their texel coordinates by the size.
- Structures: arrays of structures and structures with arrays, constructors of both, constant arrays of structures, whole-structure copies and `==` / `!=`
  (also of arrays and matrices), uniform structures with array and integer members, uniform arrays of structures, and assignments into an element chosen by a
  variable at any depth (`a[i].b[j] = ...`: each place it may be, stored where its index matches).
- Uniform blocks: their members are read like any uniform, from the stage's uniforms that the context fills from the bound buffers before each draw
  (`load_uniform_blocks`). A program without vertex attributes gets a placeholder one, sent as zeros (`kTgsiNoAttribute`).
- Clip planes (`GL_CLIP_DISTANCEi` / `GL_CLIP_PLANEi`): the last stage puts out eight distances (CLIPDIST, `GraphicsPipelineDesc::clip_distances` and
  `kFeatureClipDistances` on the device, virgl's rasterizer enables), computed as the CPU path computes them: its `gl_ClipDistance`, or the user planes (eight constant
  rows, `kTgsiClipPlanes`) against its `gl_ClipVertex`, or in the compatibility profile its `gl_Position`. A program is translated again
  with them the first time it is drawn with clip planes on.
- State the device has goes with the draw: blend factors and equations, the colour mask and the depth function (`AccelState::custom_blend`), culling, scissor and
  viewport, lines and points one pixel wide. What it has not (the stencil test, polygon offset, polygon modes and stipples, flat shading, alpha test, logic ops,
  wide lines, point sizes, two-sided colour, gl_PrimitiveID in a fragment shader, vertex streams, queries, transform feedback, framebuffer objects,
  instancing, mipmapped or non-RGBA8 textures, constant blend colours ...) keeps the draw on the CPU.
- The picture stays on the device between draws. A clear of the whole window is done there only, and the CPU's depth and stencil clears wait until the CPU needs
  them (`hw_fast_clear`, `hw_apply_pending_clears`). After the CPU drew (a draw the device could not take, or the first frame without a clear), the next draw
  the device can take writes the CPU's picture up to it first (`IAccelerator::write_color`, `AccelLimits::color_upload`, `hw_upload`) and goes on there; the depth
  stays behind, so draws with the depth test are the CPU's until a clear of the depth on the device (`hw_depth_stale_`, "the device depth is out of date"). The picture comes back to the window's image when something reads it: a swap or a front-buffer read
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
- The 3D hardware takes only part of GL (see "The 3D hardware"): the translator has no vertex streams, structure varyings between the extra stages, samplers in
  structures, `~` of unsigned integers, shifts past 23 bits, fragment `gl_PrimitiveID`, point sizes, texture kinds other than 1D / 2D / rectangle,
  vertex-shader textures, recursion, `continue` inside a `switch` or 4 x 4 `inverse`; no framebuffer objects, stencil, mipmaps or instancing on the device; the picture is read back at each swap (no zero-copy present into the
  window). Only virgl takes TGSI today (the AMD GCN compiler takes a subset).
- No `KHR_debug` callbacks (a kernel cannot call into the program), no multisampling, `wglUseFontOutlines`, overlay planes, `wglCopyContext`, GL 4.x beyond the few 4.3/4.5
  queries programs call unconditionally, compressed images are not kept as blocks (`glGetCompressedTexImage` fails).
- Wine's test still fails on rendering into DIBs (bitmap formats' read-back of GDI contents), some pixel-format choice details, and object-name expectations after sharing.
