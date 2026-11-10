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
- piglit's `.shader_test` files: `tests/run_host_tests.sh PiglitShaderTest` (pinned piglit, fetched by `tests/fetch_piglit.sh`; tens of minutes), expected failures in
  `tests/data/piglit/shader_test.xfail`.

## Not done

- GLSL: doubles, compute shaders, subroutines, SSBOs/images, implicit conversions beyond prototypes; geometry streams other than 0, transform feedback of the geometry and
  tessellation stages, layered rendering (`gl_Layer`), the compatibility varyings (`gl_FrontColor`, `gl_TexCoord`, `gl_ClipVertex`) in those stages, `gl_PrimitiveID` without a
  geometry stage; clip distances are written but not applied.
- Drawing is on the CPU (the WebGL accelerator path only takes WebGL's default framebuffer, not window surfaces).
- No `KHR_debug` callbacks (a kernel cannot call into the program), no multisampling, `wglUseFontOutlines`, overlay planes, `wglCopyContext`, GL 4.x beyond the few 4.3/4.5
  queries programs call unconditionally, compressed images are not kept as blocks (`glGetCompressedTexImage` fails).
- Wine's test still fails on rendering into DIBs (bitmap formats' read-back of GDI contents), some pixel-format choice details, and object-name expectations after sharing.
