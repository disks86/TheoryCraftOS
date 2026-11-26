# Direct3D 1, 2, 3 and 7 logic (`src/Windows/ddraw/d3d_legacy.h`)

ddraw.dll's Direct3D (IDirect3D, IDirect3D2, IDirect3D3, IDirect3D7 and their devices, viewports, lights, materials, execute buffers and
vertex buffers) is built on d3d9.dll's COM interfaces. Everything in it that is logic rather than COM objects is in one dependency-free C
header, `src/Windows/ddraw/d3d_legacy.h` (prefix `dl_` / `Dl` / `DL_`), so the device layer only has to keep objects and handles and call
d3d9. The DirectDraw part (surfaces, blits, clippers, palettes) is described in [DirectDraw.md](DirectDraw.md).

The header is plain C that is also C++, needs no C runtime and no `windows.h`, and includes only `<stddef.h>`, `<stdint.h>` and
`src/Windows/d3d9/ff_lighting.h` (for its square root, power, cosine and normal matrix). Every function is `static inline`.

## Structures

The header defines the structures of `d3dtypes.h` / `d3dcaps.h` it works with, under its own names and with the same layouts on i386 and
x86-64 (pointer members are `void *`, so a 32-bit build gets the 32-bit layout):

| d3d structure | here | | d3d structure | here |
|---|---|---|---|---|
| D3DVERTEX / D3DLVERTEX / D3DTLVERTEX | `DlVertex` / `DlLVertex` / `DlTlVertex` | | D3DEXECUTEDATA, D3DSTATUS | `DlExecuteData`, `DlStatus` |
| D3DHVERTEX | `DlHVertex` | | D3DEXECUTEBUFFERDESC | `DlExecuteBufferDesc` |
| D3DINSTRUCTION and its records | `DlInstruction`, `DlPoint`, `DlLine`, `DlTriangle`, `DlMatrixLoad`, `DlMatrixMultiply`, `DlState`, `DlProcessVertices`, `DlTextureLoad`, `DlBranch`, `DlSpan` | | D3DVIEWPORT / 2 / 7 | `DlViewport1` / `DlViewport2` / `DlViewport7` |
| D3DLIGHT (76 bytes) / D3DLIGHT2 (80) | `DlLight2` (dwSize says which) | | D3DLIGHT7 (= D3DLIGHT9) | `DlLight7` |
| D3DMATERIAL / D3DMATERIAL7 | `DlMaterial` / `DlMaterial7` | | D3DMATRIX | `DlMatrix` (`m[r][c]` is `_rc` + 1) |
| D3DTRANSFORMDATA | `DlTransformData` | | D3DDRAWPRIMITIVESTRIDEDDATA | `DlStridedData` |
| D3DPRIMCAPS | `DlPrimCaps` | | D3DDEVICEDESC (172 / 204 / 252 bytes) | `DlDeviceDesc` |
| D3DDEVICEDESC7 | `DlDeviceDesc7` | | D3DFINDDEVICESEARCH | `DlFindDeviceSearch` |

`tests/data/ddraw/d3d_legacy_layout.c` static-asserts every size and the key offsets against mingw's headers, and the constants the header
copies (opcodes, flags, render states, caps bits, error codes, the device GUIDs) against theirs; `tests/run_host_tests.sh D3dLegacyTest`
compiles it with `x86_64-w64-mingw32-gcc` and `i686-w64-mingw32-gcc`.

Results are HRESULTs (`DlResult`): `DL_OK`, `DL_DDERR_INVALIDPARAMS`, `DL_DDERR_NOTFOUND`, `DL_D3DERR_EXECUTE_FAILED`,
`DL_D3DERR_INVALIDVERTEXFORMAT`, ...

Memory comes in as views: `DlConstBytes` / `DlBytes` (`dl_const_bytes(ptr, size)`, `dl_bytes(ptr, size)`) are built once where the program's
pointer and the size it claims meet the DLL; everything else reads and writes through `dl_read`, `dl_write`, `dl_copy`, `dl_u16`, `dl_u32`, which
refuse what is outside the view.

## Execute buffers (Direct3D 1)

```c
DlResult dl_execute_data_check(const DlExecuteData *, uint32_t buffer_size);   /* SetExecuteData and Execute */
DlResult dl_execute_begin(DlExecutor *, DlConstBytes buffer, const DlExecuteData *, DlStatus *status);
int dl_execute_next(DlExecutor *, DlCommand *);      /* DL_NEXT_COMMAND, DL_NEXT_END, DL_NEXT_ERROR (x->error) */
int dl_command_point / line / triangle / span / matrix_load / matrix_multiply / state / process_vertices / texture_load
        (const DlExecutor *, const DlCommand *, uint32_t i, <record> *);     /* DL_OK, DL_RANGE (skip it), DL_BAD */
unsigned dl_triangle_edges(uint32_t flags, uint8_t pairs[6]);                  /* the edges D3DFILL_WIREFRAME draws */
int dl_execute_process(const DlProcessVertices *, DlConstBytes buffer, const DlExecuteData *, DlBytes dst, DlClipVertex *clip_out,
                       size_t clip_count, const DlTransform *, const DlLighting *, DlStatus *);
```

`dl_execute_data_check`: dwSize 48, the instructions and `dwVertexCount` vertices of 32 bytes inside the buffer (no wrapping sums), else
DDERR_INVALIDPARAMS.

The executor walks `[dwInstructionOffset, + dwInstructionLength)` and hands the device one instruction at a time (`DlCommand`: opcode,
record count, record size, offset). It does these itself, because they only touch the status (`status` is the execute buffer's
`D3DEXECUTEDATA.dsStatus`, which GetExecuteData reports):

- **EXIT** ends the stream.
- **SETSTATUS** sets `dwStatus` (D3DSETSTATUS_STATUS) and/or `drExtent` (D3DSETSTATUS_EXTENTS), as each record's dwFlags say.
- **BRANCHFORWARD** tests each record in turn: `(status & dwMask) == dwValue`, inverted by `bNegate`. A branch that is taken goes `dwOffset`
  bytes forward from the instruction's header (Wine's reading); an offset of 0 ends the stream (d3dtypes.h: "0 for exit"); a target past
  the end is an error, exactly the end is the end.
- Instructions with no records, and opcodes Direct3D does not have, are skipped (count * size bytes, as Windows and Wine do).

Errors (`DL_D3DERR_EXECUTE_FAILED`, the executor stops; what came before has been carried out): an instruction whose records go past the
area, a record size below the structure's (`dl_record_size`), a branch past the end. Running off the end of the area without EXIT is a
normal end. Every step moves forward, so no buffer can make the loop spin; the host test runs every bit flip of a buffer and thousands of
random ones under ASan/UBSan.

What the device does for each command:

| instruction | the device |
|---|---|
| POINT, SPAN | draws vertices `wFirst .. wFirst + wCount - 1` of its processed (TL) vertices as points (a span is a row of pixels, one vertex each) |
| LINE | draws the line `v1 - v2` |
| TRIANGLE | draws `v1 v2 v3`; the D3DTRIFLAG_START / STARTFLAT / ODD / EVEN flags are strip/fan hints that do not change the vertices; in D3DFILL_WIREFRAME only the edges `dl_triangle_edges` gives are drawn |
| MATRIXLOAD | copies matrix `hSrcMatrix` into `hDestMatrix` (handles from IDirect3DDevice::CreateMatrix) |
| MATRIXMULTIPLY | `dest = src1 * src2` with `dl_matrix_multiply`; an unknown handle leaves dest alone |
| STATETRANSFORM | sets the transform (`dl_transform_state`) to the matrix of the handle in `arg` |
| STATELIGHT | `dl_light_state` and `dl_lighting_apply_light_state` |
| STATERENDER | `dl_render_state(1, ...)` |
| PROCESSVERTICES | `dl_execute_process` for each record |
| TEXTURELOAD | IDirect3DTexture::Load of the two handles (unknown handles: skip the record) |

Records whose vertex indices are outside `dwVertexCount` come back as `DL_RANGE`; skip them (drawing them would read past the vertices).

`dl_execute_process` transforms (D3DPROCESSVERTICES_TRANSFORM: D3DLVERTEX, colours kept), transforms and lights (TRANSFORMLIGHT: D3DVERTEX)
or copies (COPY: D3DTLVERTEX) `dwCount` vertices from `wStart` into the device's processed vertices (`dst`, D3DTLVERTEX each) from `wDest`, and
their clip-space forms into `clip_out` (`DlClipVertex`, for the clipper; COPY'd vertices are mapped back from the screen). It merges the clip
codes into the status (union bits OR, intersection bits AND: a SETSTATUS of D3DSTATUS_DEFAULT, PROCESSVERTICES and a BRANCHFORWARD on
D3DSTATUS_CLIPINTERSECTIONALL is the classic trivial reject, and works) and, with UPDATEEXTENTS, grows `drExtent` to cover the vertices that
are on the screen (an extent with `x2 < x1`, such as the usual 2048, 2048, 0, 0, counts as empty). NOCOLOR leaves the destination's colours.
COPY does not touch the clip status.

## Transform, viewports, clipping

```c
void dl_transform_init(DlTransform *); void dl_transform_prepare(DlTransform *);
void dl_viewport7_from_v1(const DlViewport1 *, DlViewport7 *, DlMatrix *clip);
void dl_viewport7_from_v2(const DlViewport2 *, DlViewport7 *, DlMatrix *clip);
void dl_viewport1_from_v2(...); void dl_viewport2_from_v1(...);            /* GetViewport / GetViewport2 of the other form */
DlResult dl_viewport_check(size, expected, device_version, x, y, w, h, target_w, target_h);
void dl_legacy_projection(const DlMatrix *program, const DlMatrix *clip, DlMatrix *out);
uint32_t dl_clip_code(const float p[4], const float *user, uint32_t user_planes);
void dl_to_screen(...); void dl_from_screen(...);
```

`DlTransform` holds world, view, the program's projection, the viewport's clip space matrix and the D3DVIEWPORT7 rectangle (plus D3D7's user
clip planes in world space); `dl_transform_prepare` derives world*view, the total, the inverse view, the normal matrix and the camera.

The viewports follow Wine's `viewport_activate`, which its conformance tests hold against Windows:

- **D3DVIEWPORT**: the clip space scales x by `dvScaleX / (dwWidth / 2)` and y by `dvScaleY / (dwHeight / 2)`, so a projected point lands at
  `dwX + dwWidth/2 + dvScaleX * x/w`, `dwY + dwHeight/2 - dvScaleY * y/w`; depth `z/w`. SetViewport forces dvMinZ 0 / dvMaxZ 1; `dvMaxX` and
  `dvMaxY` do not clip (the rectangle does).
- **D3DVIEWPORT2**: the clip volume `dvClipX .. dvClipX + dvClipWidth`, `dvClipY - dvClipHeight .. dvClipY` (y up), `dvMinZ .. dvMaxZ` is mapped
  onto the whole rectangle and depth 0 .. 1.
- Direct3D 9 gets `program projection * clip` as its projection (`dl_legacy_projection`) and the D3DVIEWPORT7 rectangle; GetTransform returns
  the program's own matrix.

Clip codes are taken in clip space: `-w <= x, y <= w`, `0 <= z <= w` inside (D3DCLIP_LEFT .. BACK), GEN0 .. 5 for enabled user planes.

```c
unsigned dl_clip_polygon(DlClipVertex poly[DL_CLIP_MAX], unsigned count, uint32_t planes);
int dl_clip_line(DlClipVertex *a, DlClipVertex *b, uint32_t planes);
int dl_triangle_clip_class(uint32_t a, uint32_t b, uint32_t c);      /* 0 draw as is, 1 drop, 2 clip */
uint32_t dl_clip_planes(const DlTransform *);
```

d3d9 does not clip pre-transformed vertices against the near plane, so the device clips: for each triangle of processed vertices,
`dl_triangle_clip_class` of the three clip codes; if 2, copy the three `DlClipVertex` into a `DL_CLIP_MAX` array, `dl_clip_polygon` (Sutherland-
Hodgman in clip space, every attribute interpolated, new vertices computed from the inside end so shared edges are cut identically), then put
the fan on the screen with `dl_tl_from_clip` / `dl_project_fvf` and draw it. Points with a clip code are dropped; lines go through
`dl_clip_line`. With D3DEXECUTE_UNCLIPPED / D3DDP_DONOTCLIP the device may skip this.

**IDirect3DViewport::TransformVertices**: `dl_transform_vertices(t, count, in, in_stride, out, out_stride, hout, hout_count, flags,
&offscreen)` with the viewport's transform. As Windows does (Wine's tests): clip codes are worked out with D3DTRANSFORM_CLIPPED or
UNCLIPPED; with CLIPPED a clipped vertex gets clip-space x y z w in `lpOut` instead of screen coordinates, `lpHOut` gets its codes and x y z
before the viewport's clip space, and `*offscreen` is the codes every vertex shares; the bytes after the first 16 of each input vertex are
copied to the output. DDERR_INVALIDPARAMS for neither flag, a missing lpHOut with CLIPPED, or views too small. (LightElements is
DDERR_UNSUPPORTED, as in Wine.)

## Lighting and vertex processing

```c
void dl_lighting_init(DlLighting *, unsigned version);
void dl_light_vertex(const DlTransform *, const DlLighting *, position, normal, vertex_diffuse, vertex_specular, float diffuse[4], float specular[3]);
void dl_process_vertex(t, l, int light, position, normal, const uint32_t *diffuse, const uint32_t *specular, const float (*tex)[4], unsigned sets, DlClipVertex *);
DlResult dl_process_fvf(t, l, int light, uint32_t fvf, DlConstBytes in, uint32_t count, DlClipVertex *out, size_t out_count, DlProcessResult *);
DlResult dl_project_fvf(t, uint32_t fvf, const DlClipVertex *, uint32_t count, DlBytes out);   /* as dl_output_fvf(fvf) vertices */
void dl_tl_from_clip(...); void dl_clip_from_tl(...);
void dl_result_init / dl_result_add; void dl_status_merge(DlStatus *, const DlProcessResult *, int update_extents);
float dl_fog_factor(mode, start, end, density, distance);
```

`DlLighting` is what the device keeps for the CPU lighting: whether it is the Direct3D 1-3 model (`legacy`), whether lighting is on (1-3: a
material is set; 7: D3DRENDERSTATE_LIGHTING), the material, the global ambient, SPECULARENABLE, LOCALVIEWER, NORMALIZENORMALS, COLORVERTEX and
the four material sources, vertex fog (mode, start, end, density, range fog), and up to `DL_LIGHTS` (32) lights in world space
(`DlLightEntry`). `dl_lighting_init` sets each version's starting values.

The model (both versions): colour = emissive + ambient * (global + sum of light ambients) + diffuse * sum(att * spot * max(N.L, 0) * light
diffuse); specular = specular * sum(att * spot * (N.H)^power * light specular) for lights with N.L > 0 when SPECULARENABLE is on; H half way
between L and the direction to the camera (from the vertex with a local viewer). The differences of Direct3D 1-3:

- **attenuation** of point and spot lights is `a0 + a1 r + a2 r^2` with `r = (range - d) / range` (multiplied, not divided; a0 = 0, a1 = 1
  fades to nothing at the range) - the legacy attenuation wined3d implements for ddraw; Direct3D 7 uses `1 / (a0 + a1 d + a2 d^2)`. No light
  past the range either way.
- **parallel point lights** shine from their position onto the whole object: the direction is from the object's origin (the world matrix's
  translation), the same for every vertex, unattenuated (Direct3D's documentation). *Wine differs here*: wined3d aims them from the camera
  (the normalized view-space position).
- lights have one colour: diffuse and, unless D3DLIGHT_NO_SPECULAR, specular; no ambient (`dl_light_from_legacy`).
- vertex fog (D3DLIGHTSTATE_FOGMODE) is part of the lighting module: only lit vertices get it; a D3DLVERTEX keeps its own specular alpha.

Fog goes into the specular alpha (1 = no fog); without fog the alpha is the vertex's own, 255 for a D3DVERTEX (Wine writes 0 there).

`dl_process_fvf` is the general form, for IDirect3DDevice2/3 DrawPrimitive of untransformed vertices (D3DVT_* through
`dl_fvf_of_vertex_type`, or FVF codes) and Direct3D 3/7 vertex buffers' ProcessVertices: vertices with a normal are lit when `light` is set (not
D3DDP_DONOTLIGHT / D3DVOP_LIGHT) and the lighting is on; vertices without colours are white with a black specular; XYZRHW vertices pass
through. Draw the result as `dl_output_fvf(fvf)` (XYZRHW, both colours, the input's texture sets) through `dl_project_fvf`, clipping first as
above. The CPU path is the faithful one for Direct3D 1-3 (d3d9's fixed function has neither the legacy attenuation nor parallel point lights);
Direct3D 7 geometry can go to d3d9's fixed function directly.

**Verified against Wine's ddraw**: `tests/data/ddraw/wine_reference.c` draws 48 scenes (from `wine_scenes.h`, shared with the test) with
Wine's ddraw.dll - IDirect3DVertexBuffer::ProcessVertices with a D3DVIEWPORT2, legacy point / spot / directional / parallel point lights and a
material, and IDirect3DViewport3::TransformVertices with a D3DVIEWPORT - and `wine_reference.txt` is its output. The host test reproduces every
screen position, clip code, D3DHVERTEX and TransformVertices output, and every colour and specular colour within 2/255, the parallel point
lights made Wine's camera-aimed form for the comparison.

## Caps and devices

```c
void dl_device_desc7(const DlCaps9 *, int kind, DlDeviceDesc7 *);               /* IDirect3D7::EnumDevices, IDirect3DDevice7::GetCaps */
void dl_device_descs(const DlCaps9 *, int kind, unsigned version, DlDeviceDesc *hal, DlDeviceDesc *hel);    /* EnumDevices 1-3, GetCaps */
void dl_device_desc_from7(const DlDeviceDesc7 *, unsigned version, DlDeviceDesc *);
int dl_enum_device(unsigned version, unsigned index);           /* DL_DEVICE_*, DL_DEVICE_NONE past the end */
const DlGuid *dl_device_guid(int kind); int dl_device_kind(const DlGuid *);
const char *dl_device_name(int kind); const char *dl_device_description(int kind);
int dl_device_creatable(unsigned version, int kind);
DlResult dl_find_device(const DlCaps9 *, unsigned version, const DlFindDeviceSearch *, DlBytes result);
```

`DlCaps9` is the subset of D3DCAPS9 the descriptions come from (same field names: copy them from IDirect3DDevice9::GetDeviceCaps), plus
`color_key` (set it when the device implements colour-keyed textures: it becomes D3DPTEXTURECAPS_TRANSPARENCY).

The D3DDEVICEDESC7 is Wine's `ddraw_get_d3dcaps`: every field masked to the bits Direct3D 7 defines, ZBIAS from DEPTHBIAS, ANTIALIASEDGES
from antialiased lines, POW2 | NONPOW2CONDITIONAL for a device without a power-of-two limit, render depths 16/24/32, depth buffers 16/24,
texture blend caps all eight, 32x32 stipple, min texture 1x1, user clip planes at most 32, and in addition to Wine the legacy D3DFILTER_*
filter caps that follow from the MIN/MAG/MIP ones, the flat shading caps wherever Gouraud is, and VERTEXFOG from FOGVERTEX. Per device:
RGB / MMX / Ramp / reference lose HWTRANSFORMANDLIGHT, DRAWPRIMITIVES2EX and HWRASTERIZATION; HAL loses HWTRANSFORMANDLIGHT; T&L HAL keeps
all.

The D3DDEVICEDESC (Wine's `ddraw_d3dcaps1_from_7`) has dwSize 172 (Direct3D 1), 204 (2) or 252 (3) and zeros past it; dwFlags all eleven
D3DDD_*, colour model RGB, transform caps CLIP, bClipping, lighting caps point / spot / directional / parallel point with the RGB model and
MaxActiveLights lights, dwMaxVertexCount 2048. `dl_device_descs` gives the HAL and HEL pair EnumDevices passes (Wine's `enum_devices_d3d3`):

| device | hardware description | software description |
|---|---|---|
| HAL | complete | no colour model, no HWTRANSFORMANDLIGHT / DRAWPRIMITIVES2EX, no POW2 / NONPOW2CONDITIONAL / PERSPECTIVE |
| RGB, MMX, Ramp | no colour model, no flags, no hardware device caps, no POW2 / NONPOW2CONDITIONAL / PERSPECTIVE | complete but the hardware device caps; colour model RGB (Ramp: MONO) |

(Rollcage tells emulation from HAL by POW2 being in the software caps of the one and the hardware caps of the other.)

Enumeration order (Wine's): IDirect3D and IDirect3D2: Ramp Emulation, RGB Emulation, Direct3D HAL; IDirect3D3: RGB Emulation, Direct3D HAL;
IDirect3D7: RGB Emulation, Direct3D HAL, Direct3D T&L HAL. Names are Windows' ("Ramp Emulation", "RGB Emulation", "MMX Emulation",
"Direct3D HAL", "Direct3D T&L HAL"), descriptions Microsoft's. `dl_device_creatable`: HAL, RGB and the reference device always; MMX before 7;
Ramp for 1 and 2; T&L HAL for 7 only. MMX and Ramp devices are drawn as RGB (as DirectX 6 and later do).

`dl_find_device` is IDirect3D/2/3::FindDevice: the search must be 92 bytes and the result one of 364 / 428 / 524 bytes (its dwSize), else
DDERR_INVALIDPARAMS. It goes through the version's list (1: HAL, RGB, Ramp; 2: HAL, MMX, RGB, Ramp; 3: HAL, RGB) and takes the first device
that matches the GUID (D3DFDS_GUID), hardware or not (D3DFDS_HARDWARE), the colour model and every caps field the flags name (in the line caps
with D3DFDS_LINES, else the triangle caps; of the hardware description for the HAL, the software one otherwise). The result gets the
device's GUID and its two descriptions at the offsets of the result's size. DDERR_NOTFOUND when nothing matches. (Wine returns its own GUID
and ignores the caps; this is Windows' behaviour.)

## States

Every mapping fills a `DlStateOps` (cleared first) with what to call on d3d9, in order:

| op | call |
|---|---|
| `DL_SET_RENDER` | `SetRenderState(state, value)` |
| `DL_SET_STAGE` | `SetTextureStageState(index, state, value)` |
| `DL_SET_SAMPLER` | `SetSamplerState(index, state, value)` |
| `DL_SET_TEXTURE` | `SetTexture(index, texture of legacy handle value)`, 0 unbinds; DDERR_INVALIDPARAMS for a handle that is not a texture |
| `DL_SET_MATERIAL` | the material of handle `value` becomes current (0: none); DDERR_INVALIDPARAMS for one that is not a material |

```c
DlResult dl_render_state(unsigned version, uint32_t state, uint32_t value, const DlStateContext *, DlStateOps *);
int dl_render_state_get_override(unsigned version, uint32_t state, uint32_t *value);
int dl_render_state_default(unsigned version, uint32_t state, int has_depth, uint32_t *value);
DlResult dl_texture_map_blend(uint32_t mode, uint32_t previous, int texture, DlStateOps *);
DlResult dl_light_state(uint32_t state, uint32_t value, DlStateOps *);
int dl_light_state_default(uint32_t state, uint32_t *value);
DlResult dl_texture_stage_state(uint32_t stage, uint32_t state, uint32_t value, DlStateOps *);
uint32_t dl_texture_stage_default(uint32_t stage, uint32_t state);
uint32_t dl_transform_state(unsigned version, uint32_t state);     /* Direct3D 9's D3DTS_*, 0 = DDERR_INVALIDPARAMS */
uint32_t dl_depth_bias(uint32_t zbias, uint32_t depth_bits);
```

The device stores every legacy render state value as set (GetRenderState returns it; `dl_render_state_get_override` gives the few that read
back otherwise), and `DlStateContext` tells the mapping what it needs: the current WRAP0, the current TEXTUREMAPBLEND, whether stage 0 has a
texture and whether it has alpha, and the depth buffer's bits.

Render states (`dl_render_state`):

- same number in Direct3D 9: ZENABLE .. TEXTUREFACTOR, WRAP0-7, CLIPPING .. CLIPPLANEENABLE (ALPHABLENDENABLE = BLENDENABLE);
- EDGEANTIALIAS -> ANTIALIASEDLINEENABLE; ANTIALIAS (any mode) -> MULTISAMPLEANTIALIAS; ZBIAS -> DEPTHBIAS `-zbias / 2^bits` (as d3d8.dll);
- sampler 0: TEXTUREADDRESS (U and V), TEXTUREADDRESSU/V, BORDERCOLOR, MIPMAPLODBIAS, ANISOTROPY (MAXANISOTROPY), TEXTUREMAG
  (`dl_mag_filter`), TEXTUREMIN (`dl_min_filter`: NEAREST point/none, LINEAR linear/none, MIPNEAREST point/point, MIPLINEAR linear/point,
  LINEARMIPNEAREST point/linear, LINEARMIPLINEAR linear/linear - Wine's tables; `dl_legacy_min_filter` / `dl_legacy_mag_filter` are the
  reverse for GetRenderState if the device prefers it);
- WRAPU / WRAPV set bit 0 / 1 of WRAP0;
- TEXTUREHANDLE -> `DL_SET_TEXTURE` (and the device maps TEXTUREMAPBLEND again: the texture's alpha matters to MODULATE);
- TEXTUREMAPBLEND -> `dl_texture_map_blend`;
- kept with no d3d9 call: TEXTUREPERSPECTIVE (d3d9 is always perspective-correct), LINEPATTERN, MONOENABLE, ROP2, PLANEMASK, SUBPIXEL/X,
  STIPPLEDALPHA, STIPPLEENABLE, STIPPLEPATTERN00-31, FLUSHBATCH, TRANSLUCENTSORTINDEPENDENT, EXTENTS, and what the device must implement
  itself: ZVISIBLE, COLORKEYENABLE, COLORKEYBLENDENABLE (see below). The caps do not claim stippling, ROP2 or line patterns;
- Direct3D 1-3: LIGHTING, NORMALIZENORMALS and LOCALVIEWER are accepted and do nothing; they read back 0xFFFFFFFF (Wine);
- DDERR_INVALIDPARAMS: state 0, states of 256 and above (D3DSTATE_OVERRIDE_BIAS), TEXTUREHANDLE and TEXTUREMAPBLEND on Direct3D 7 (Wine), a
  TEXTUREMAPBLEND that is not a mode. Other numbers below 256 are kept (the runtime passes them to the driver).

Texture map blends (`dl_texture_map_blend`, stage 0; Ct/At the texture, Cf/Af the diffuse colour):

| mode | colour | alpha |
|---|---|---|
| DECAL, COPY | Ct | At |
| MODULATE | Ct Cf | At if the texture has alpha, else Af (Wine's `fixup_texture_alpha_op`) |
| MODULATEALPHA | Ct Cf | At Af |
| DECALALPHA | lerp(Cf, Ct, At) | Af |
| ADD | Ct + Cf | Af |
| DECALMASK | lerp(Cf, Ct, At) | Af |
| MODULATEMASK | stage 0 lerp(1, Ct, At) with its constant white, stage 1 * Cf | Af |

With no texture every mode passes the diffuse colour. The mask modes select by the texture alpha's top bit; the lerp is exact for the 1-bit
alpha textures they are used with. Leaving MODULATEMASK disables stage 1 again. (Wine does not implement the two mask modes.)

Defaults (`dl_render_state_default`): Direct3D 9's, with ddraw's own: COLORKEYENABLE on for Direct3D 1, SPECULARENABLE on for 2,
TEXTUREMAPBLEND MODULATE and NORMALIZENORMALS on before 7, ZENABLE on when a depth buffer is attached, TEXTUREPERSPECTIVE on, filters NEAREST,
addressing WRAP, ROP2 R2_COPYPEN, PLANEMASK all ones.

Light states (`dl_light_state`): MATERIAL -> `DL_SET_MATERIAL`, AMBIENT -> AMBIENT, FOGMODE -> FOGVERTEXMODE, FOGSTART/END/DENSITY -> the fog
states, COLORVERTEX -> COLORVERTEX; COLORMODEL takes RGB and MONO and does nothing (GetLightState reports RGB); 0 or past COLORVERTEX, or
another colour model: DDERR_INVALIDPARAMS. `dl_lighting_apply_light_state` feeds the same values to the CPU lighting.

Texture stage states (`dl_texture_stage_state`, Direct3D 3 and 7): ADDRESS (U and V), ADDRESSU/V, BORDERCOLOR, MAGFILTER (D3DTFG_POINT,
LINEAR, FLATCUBIC -> PYRAMIDALQUAD, GAUSSIANCUBIC -> GAUSSIANQUAD - the d3d9 filters with the same caps bits - ANISOTROPIC; unknown -> point),
MINFILTER (same numbers), MIPFILTER (D3DTFP_NONE/POINT/LINEAR; 0 -> linear, unknown -> none, as Wine), MIPMAPLODBIAS, MAXMIPLEVEL,
MAXANISOTROPY are the stage's sampler states; the others keep their numbers; states 0 and past TEXTURETRANSFORMFLAGS do nothing; stages past
7 are DDERR_INVALIDPARAMS. A Direct3D 3 SetTextureStageState also ends the legacy texture blending (the device sets its TEXTUREMAPBLEND
context to 0).

Transform states (`dl_transform_state`): WORLD -> 256, VIEW 2, PROJECTION 3 (with the clip space for 1-3), and for 7 WORLD1-3 -> 257-259 and
TEXTURE0-7 -> 16-23.

## Conversions

- `dl_light_from_legacy(const DlLight2 *, DlLightEntry *, int *active)`: D3DLIGHT (76) / D3DLIGHT2 (80); a D3DLIGHT is always active, a
  D3DLIGHT2 when D3DLIGHT_ACTIVE is set; types POINT .. PARALLELPOINT (GLSPOT and others DDERR_INVALIDPARAMS, as Wine).
- `dl_light_from_light7`: D3DLIGHT7, POINT / SPOT / DIRECTIONAL.
- `dl_light7_from_legacy`: a legacy light as a D3DLIGHT7 / D3DLIGHT9 for a device that wants d3d9's fixed function; returns 0 where d3d9
  cannot light it the same (parallel point lights, attenuation that is not constant).
- `dl_material7_from_legacy`: D3DMATERIAL (dwSize 80) -> D3DMATERIAL7; hTexture and dwRampSize stay with the material.
- FVF: `dl_fvf_valid(fvf, version)` (a position, no reserved bits 0x1 / 0xF000, at most 8 sets, D3DFVF_TEXCOORDSIZEn only in 7 and only for
  sets the vertex has; RESERVED1 is D3DLVERTEX's reserved word and allowed), `dl_fvf_layout` (offsets of every part), `dl_fvf_stride`,
  `dl_fvf_elements` (a D3DVERTEXELEMENT9 declaration: d3d9 has no RESERVED1 - its 0x20 is PSIZE - so the reserved word is skipped by giving
  d3d9 a declaration rather than the code), `dl_fvf_of_vertex_type` (D3DVT_VERTEX 0x112, LVERTEX 0x1E2, TLVERTEX 0x1C4; 0 otherwise),
  `dl_output_fvf`.
- `dl_gather_strided(fvf, strided, first, count, available, out)`: DrawPrimitiveStrided's per-part pointers into interleaved vertices (the
  position stream carries the blend weights; a stride of 0 repeats; `available` is how many vertices each pointer may be read for: the views
  are built from it).
- `dl_primitive_count(type, vertices)`: points n, lines n/2, strip n-1, triangles n/3, strips and fans n-2; 0 for too few or an unknown type
  (the device returns D3DERR_INVALIDPRIMITIVETYPE / DDERR_INVALIDPARAMS).
- `dl_sphere_visibility7` (IDirect3DDevice7::ComputeSphereVisibility: D3DSTATUS_CLIPUNION*/CLIPINTERSECTION* bits for the six planes and the
  enabled user planes) and `dl_sphere_visibility3` (IDirect3DDevice3: D3DVIS_* per plane and for the frustum), Wine's algorithm: the planes of
  world * view * projection (with the clip space), touching counts as crossing in 3 but not in 7. User planes are in world space: the centre is
  moved by the world matrix and the radius scaled by its largest axis.

## What the device layer does itself

- Objects and handles: matrices (CreateMatrix / SetMatrix / GetMatrix / DeleteMatrix), materials, textures (GetHandle), lights attached to
  viewports, the current viewport, execute buffers (Lock / Unlock / SetExecuteData / GetExecuteData; Execute while locked is
  D3DERR_EXECUTE_LOCKED), the processed-vertex buffer of an execute buffer (sized by dwVertexCount) and its `DlClipVertex` copy.
- Drawing: the processed vertices through d3d9 as XYZRHW vertices, clipping as described above; D3DFILL_WIREFRAME edges from the triangle
  flags.
- **D3DRENDERSTATE_ZVISIBLE**: clearing D3DSTATUS_ZNOTVISIBLE when what is drawn with it would pass the depth test needs the GPU: an occlusion
  query (d3d9 has them) around the primitives drawn while ZVISIBLE is set. The executor only reports the status the device leaves.
- **Colour keying** (COLORKEYENABLE, on by default for Direct3D 1; COLORKEYBLENDENABLE): turn keyed texels into alpha 0 and alpha-test them
  away (d3d9 has no colour key); then set `DlCaps9.color_key`.
- Lights: collect the active lights of the current viewport into `DlLighting` (in world space); Direct3D 1-3 lighting needs a material
  (`DlLighting.enabled`).
- Extents (D3DRENDERSTATE_EXTENTS, D3DDP_DONOTUPDATEEXTENTS, GetClipStatus / SetClipStatus) from `DlProcessResult`.
- The FPU mode of DDSCL_FPUSETUP / FPUPRESERVE.

## Open gaps

- Ramp (D3DCOLOR_MONO) devices draw in RGB: the ramp rasterizer's palette ramps (D3DMATERIAL.dwRampSize, hTexture) are not emulated, as in
  DirectX 6 and later runtimes and in Wine.
- Line patterns, stippling, ROP2 and plane masks have no Direct3D 9 form; the states are kept and the caps do not claim them.
- The mask texture blends are exact only for textures whose alpha is 0 or 1 (the top bit selects on Windows).
- Not verified against Windows itself (no Windows here): the reference is Wine's ddraw run under Wine (positions, clip codes, lighting) and
  Wine's code and tests for the rest. Where this header follows Microsoft's documentation instead of Wine (parallel point lights from the
  object's origin, FindDevice matching caps and returning the device's GUID, the specular alpha of 255 without fog, the mask blends), that is
  said above.
