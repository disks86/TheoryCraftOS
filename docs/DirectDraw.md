# DirectDraw

`ddraw.dll` (src/Windows/ddraw) is DirectDraw for Windows programs: `IDirectDraw`, `IDirectDraw2`, `IDirectDraw4` and `IDirectDraw7`, their
surfaces (`IDirectDrawSurface` .. `IDirectDrawSurface7`, `IDirectDrawGammaControl`), palettes and clippers, written from DirectDraw's documentation
and the behaviour Wine's conformance tests record for Windows (Wine's implementation was read for the order of its checks). It is built for 64-bit
and for 32-bit programs ([Wow64.md](Wow64.md)); most DirectDraw programs are 32-bit. Direct3D 1 .. 7, which live in ddraw.dll on Windows, are a
separate part of the same library that plugs into the hooks below (see "The Direct3D part").

## Files

| file | what |
|---|---|
| `ddraw.c` | the object layouts (`DDraw`, `Surface`, `SurfaceGroup`, `Palette`, `Clipper`), the global lock, the Direct3D hooks (`DdD3dHooks`), `DirectDrawCreate(Ex)`, `DirectDrawCreateClipper`, the enumeration exports, the class factory (`CLSID_DirectDraw`, `CLSID_DirectDraw7`, `CLSID_DirectDrawClipper`), `DllRegisterServer` |
| `ddraw_pixels.h` | dependency-free: the formats, sizes and pitches, raw pixels, colour conversions (RGB, palettes, luminance, alpha, YUY2/UYVY/YV12/NV12, DXT1-5), fills, the blitter, the A8R8G8B8 read-out. Host test: `tests/DdrawPixelsTest.cpp` |
| `formats.inc` | `DDPIXELFORMAT` <-> the formats of ddraw_pixels.h, `DDSURFACEDESC` <-> `DDSURFACEDESC2` |
| `display.inc` | display modes, lost surfaces, the focus window, the cooperative level, vertical blank and scan line (D3DKMT), caps, device identifier |
| `ddobj.inc` | the IDirectDraw methods of every version |
| `surface.inc` | surface creation, references and groups, attachments, Lock, GetDC, colour keys, palettes and clippers of a surface, private data, Restore, SetSurfaceDesc, priority and LOD, the gamma ramp |
| `blit.inc` | Blt, BltFast, BltBatch, Flip |
| `present.inc` | the primary surface on the screen |
| `palclip.inc` | IDirectDrawPalette, IDirectDrawClipper |
| `vtbl.inc` | the vtables of every interface version (filled by name: the compiler checks each method against mingw's ddraw.h) |
| `legacy.inc` | `AcquireDDThreadLock`, `ReleaseDDThreadLock`, `GetSurfaceFromDC`, `D3DParseUnknownCommand`, `GetDDSurfaceLocal`, `DDInternalLock/Unlock`, `DDGetAttachedSurfaceLcl`, `CompleteCreateSysmemSurface`, `GetOLEThunkData`, `RegisterSpecialCase`, `SetAppCompatData`, `DSoundHelp` |
| `ddraw.def`, `stubs/ddraw.inc` | the exports (all implemented) |

## How it works

| part | here |
|---|---|
| references | each DirectDraw interface counts its own references; the object is destroyed when all four are 0 (the cooperative level goes back to normal, the display mode to the desktop's, a device window it made is destroyed). A surface or palette made through IDirectDraw4/7 holds a reference of that interface; one made through IDirectDraw/2 none. The memory of an object lives until its last surface group and palette are gone, so a surface of a destroyed object is never a dangling pointer. A surface counts each of its interfaces' references (1, 2, 3, 4, 7, gamma); IUnknown is IDirectDrawSurface. |
| attachments | AddAttachedSurface links (`AttachLink`, each holding a reference of the interface it was given): a surface may be attached to several (a device's z buffer to a second target, as Windows), not to one it has attachments of its own from; z buffers only of the surface's size (DDERR_CANNOTATTACHSURFACE). |
| groups | the surfaces of one CreateSurface (a flipping chain, the levels of a mipmap, the faces of a cube map) are a group: the root's last release destroys the group's structure (its attachments, palette); a sublevel or back buffer the program still holds stays usable until its own last release, then the group's memory goes. |
| identities | a surface pointer a program passes in may be any version's interface (Windows takes them, Wine's tests pass them): `surface_from_any` finds the surface by its vtable. QueryInterface answers the versions, `IDirectDrawGammaControl`, `E_NOINTERFACE` for `IDirectDrawColorControl` (no colour controls), and for unknown interfaces `E_NOINTERFACE` on version 7 surfaces, `E_INVALIDARG` on the older ones, as Windows. `IDirectDraw3` is not an interface (E_NOINTERFACE). |
| initialization | DirectDrawCreate(Ex) objects are initialized; CoCreateInstance's answer DDERR_NOTINITIALIZED until `Initialize` (then DDERR_ALREADYINITIALIZED). |
| cooperative level | Windows' rules (see `ddraw_set_cooperative_level`): NORMAL / EXCLUSIVE / SETFOCUSWINDOW, EXCLUSIVE needs FULLSCREEN and a window, CREATEDEVICEWINDOW (a `DirectDrawDeviceWnd` popup) needs a focus window, a second exclusive object gets DDERR_EXCLUSIVEMODEALREADYSET. Exclusive mode sizes the window to the screen (shown when it is active or in the foreground, topmost when in the foreground; DDSCL_NOWINDOWCHANGES: not at all), clips the cursor, and puts ddraw's window procedure in front of the focus window's: WM_ACTIVATEAPP off minimizes the window, puts the desktop's mode back and loses every surface, on puts the program's mode back. The program's procedure comes back only while ddraw's is still the window's (as Windows). Window changes of a window of another thread are made by a thread of their own (that thread may be waiting for the caller). |
| display modes | user32's EnumDisplaySettings / ChangeDisplaySettingsEx; the kernel lists every size at 32, 16 and 8 bits and keeps the depth a program set (`Win32kUserMiscMonitor.cpp`), while scanning out 32 bits. EnumDisplayModes lists them (unique sizes and depths, with DDEDM_REFRESHRATES also by rate), SetDisplayMode takes 8, 15, 16, 24 and 32 bits, a width or height of 0 is DD_OK and changes nothing (as Windows). A mode change by anyone (ChangeDisplaySettings too) loses the surfaces that can be lost. |
| lost surfaces | primary surfaces, z buffers and video memory surfaces; IsLost, Restore (DDERR_WRONGMODE for a primary of another mode; the root restores every surface made with it), RestoreAllSurfaces, TestCooperativeLevel. Leaving exclusive mode for DDSCL_NORMAL loses them in IDirectDraw2 and later (not for DDSCL_NORMAL with DDSCL_FULLSCREEN, not in IDirectDraw's SetCooperativeLevel), the object staying fine (`test_lost_device` of each version). A program that loses the foreground hears WM_ACTIVATE before ddraw's hook acts at WM_ACTIVATEAPP: the desktop's mode first, then the loss, then the window minimized, then the program's WM_ACTIVATEAPP. Their memory is kept (a restored surface's contents are formally undefined). |
| memory | every surface's pixels are system memory and what counts. A surface without memory caps goes to "video memory" when its format is one the hardware path takes (not palettes, not 3-3-2, not YUV textures), else to system memory (and can be lost, as Windows' fallback can). Pitches: 8-byte rows, 4 for system memory of a 64-bit process (Windows' numbers); block formats report a linear size. GetAvailableVidMem counts the video memory surfaces. DDSD_LPSURFACE (version 4 on) uses the program's memory; SetSurfaceDesc changes it. DuplicateSurface makes a surface object that shares the memory. |
| formats | P1/P2/P4/P8, R3G3B2, A8R3G3B2, R5G6B5, X1R5G5B5, A1R5G5B5, X4R4G4B4, A4R4G4B4, R8G8B8, X8R8G8B8, A8R8G8B8, X8B8G8R8, A8B8G8R8, A2R10G10B10, A2B10G10R10, A8, L8, A8L8, A4L4, L16, V8U8, L6V5U5, X8L8V8U8, Q8W8V8U8, V16U16, D16, D15S1, D24X8 (both masks), D24S8, D24X4S4, D32, YUY2, UYVY, YV12, NV12, DXT1-5 (GetFourCCCodes lists the FourCCs). |
| Lock | rectangles checked as Windows (DDERR_INVALIDPARAMS; block-aligned for DXT in video memory, while system memory and managed DXT surfaces take a part of a block, as Windows Vista on), a second lock or a lock while a DC is out DDERR_SURFACEBUSY, Unlock of an unlocked surface DDERR_NOTLOCKED; any of DDSURFACEDESC/2's sizes, and a version 7 system memory texture takes any size (as Windows). |
| GetDC | as Windows' runtime: `D3DKMTCreateDCFromMemory` makes a GDI memory DC whose DIB is the surface's own memory (top-down, 16-bit formats with bit fields; GetObject reports a positive height and no image size, as Windows), so GDI draws straight into the surface. Formats: RGB of 16, 24 and 32 bits and P8; P8 takes the surface's palette, else the primary's, else (8-bit display mode only) the system palette, and keeps those colours until ReleaseDC (a later SetEntries or SetPalette does not change the DC, as Windows). Other formats DDERR_CANTCREATEDC. GetSurfaceFromDC (the methods and the export) finds the surface. |
| Blt | colour fill (raw value), depth fill (depth bits; stencil kept), SRCCOPY/BLACKNESS/WHITENESS ROPs (others DDERR_NORASTEROPHW, as Windows' blitter), stretch and shrink (point sampling), DDBLTFX mirroring and rotation by 90/180/270, source keys (the surface's or the override), destination keys (versions 4 and 7: the destination's own; versions 1-3: the source's DDCKEY_DESTBLT, which is what Wine's test records for Windows), conversion between all colour formats (YUV and DXT sources decoded, a DXT source's alpha landing in an X8R8G8B8 destination's unused byte; a palette destination takes the nearest entry; a P8 source into a colour format copies the index into every channel - Windows' blitter does not look the palette up, `test_p8_blit`; X1R5G5B5 and A1R5G5B5, X4R4G4B4 and A4R4G4B4 copy raw values between each other), DXT destinations of any rectangle, stretched or converted (the blocks touched are decoded, written and encoded again - `dd_bc_encode_block`, two colours of a block exact to 5-6-5, as Windows' results in `test_surface_format_conversion_alpha`), depth to depth, blits of a surface onto itself (overlapping parts copied away first), the destination's clipper (each rectangle of its clip list, the source scaled to match). DDBLT_ALPHA*/DDBLT_ZBUFFER* are dropped as Windows' software blitter drops them. Between surfaces of two DirectDraw objects only copies of one format: a conversion is E_NOTIMPL (`test_cross_device_blt`); no BLACKNESS/WHITENESS on YUV or DXT surfaces (DDERR_NORASTEROPHW; a colour fill of the raw value works). BltFast: no clipper (DDERR_BLTFASTCANTCLIP), the source and destination keys. BltBatch: DDERR_UNSUPPORTED - Windows never implemented it. |
| flipping chains | primary, off-screen and texture chains (a chain of textures is made, as Windows; Wine refuses it). On 64-bit Windows IDirectDraw7 has no Direct3D and refuses textures with E_NOINTERFACE: the same here while no Direct3D part is registered (IDirectDraw4 and older make them). |
| Flip | the chain's memory (and Direct3D shadows) go round, or the front and a target exchange theirs; the primary is shown, then Flip waits for 1-4 vertical blanks (DDFLIP_INTERVALn, none with DDFLIP_NOVSYNC). Flipping the primary needs exclusive mode. |
| presentation | present.inc: every write into the primary's front buffer goes to the screen through GDI's SetDIBitsToDevice, converted to 32 bits (8-bit primaries through their palette, 16-bit ones converted), with the gamma ramp. Normal level: screen coordinates, to the screen or into a clipper's window (its client area). Exclusive mode: the window's whole DC (the window covers the screen). A new windowed 32-bit primary starts with the screen's contents. |
| palettes | 2/4/16/256 entries, DDPCAPS_8BITENTRIES, ALLOW256 (else entries 0 and 255 are black and white), ALPHA (only on textures), PRIMARYSURFACE while it is the primary's; SetEntries updates Direct3D's copies and the screen (not a DC already given out). |
| clippers | SetHWnd (the window's visible region, GetRandomRgn), SetClipList (DDERR_CLIPPERISUSINGHWND with a window), GetClipList (DDERR_REGIONTOOSMALL, E_FAIL for a destroyed window), IsClipListChanged (the window's visible rectangle against the last answer). Like Windows', methods check that the pointer is a live clipper (DDERR_INVALIDPARAMS / 0). |
| caps | HAL: blits, fills, stretching, mirroring, rotation, colour keys, clipping, system memory blits, FourCC blits, palettes, flipping, mipmaps, z buffers of 16/24/32 bits, primary gamma, flip intervals, 3D devices when the Direct3D part is in; HEL: the software layer's set (as Wine's test records Windows' HEL). No overlays (DDERR_NOOVERLAYHW, DDERR_NOTAOVERLAYSURFACE), no colour controls, no stereo, no video ports. |
| GetGDISurface | the surface of the primary's chain that has the GDI's memory now (Flip passes it round: after one flip the back buffer); FlipToGDISurface flips it back to the front, DDERR_NOTFLIPPABLE for a primary without a chain. |
| the rest | GetMonitorFrequency, GetScanLine (DDERR_VERTICALBLANKINPROGRESS in the blank), GetVerticalBlankStatus, WaitForVerticalBlank (BLOCKBEGIN, BLOCKEND; BLOCKBEGINEVENT DDERR_UNSUPPORTED as Windows), StartModeTest/EvaluateMode (each mode set for 15 seconds or until passed/failed, DDERR_TESTFINISHED at the end, the desktop's mode back), Compact (DDERR_NOEXCLUSIVEMODE outside exclusive mode), EnumSurfaces (DOESEXIST all/match/nomatch, CANBECREATED), GetDeviceIdentifier (d3d9.dll's adapter; only up to dwWHQLLevel is written), private data, uniqueness values (they change with every write), PageLock/PageUnlock (counted for system memory surfaces: DDERR_NOTPAGELOCKED), priority and LOD of managed textures. |

## Layers below it that this work changed

| layer | change |
|---|---|
| `Win32kUserMiscMonitor.cpp` | display modes at 32, 16 and 8 bits; ChangeDisplaySettings keeps the depth a program set (scanout stays 32). |
| `Win32kGdiDib.cpp` | `NtGdiDdDDICreateDCFromMemory` / `DestroyDCFromMemory` (a DIB on the program's memory; the colour table from PALETTEENTRYs). |
| `Win32kGdiBlt.cpp`, `Win32kGdiDraw.cpp` | ROPs on a 32 bpp BI_RGB DIB of the program's work on the whole pixel (`rop3_apply_bits`): WHITENESS sets the fourth byte, BLACKNESS and brushes clear it, SRCCOPY from a 32 bpp source copies it, as Windows. Screen and window surfaces keep their own. |
| `Win32kGdiObjects.cpp` | `NtUserSelectPalette` (was not registered: SelectPalette always returned NULL). |
| `Win32kUserInput.cpp` | WM_ACTIVATEAPP to the top-levels of the threads that lose and gain the foreground; a hidden top-level can be made the foreground window (as Windows). The thread that loses the foreground gets its active window's WM_NCACTIVATE and WM_ACTIVATE (WA_INACTIVE) before WM_ACTIVATEAPP (FALSE), then WM_KILLFOCUS - Windows' order (`test_coop_level_mode_set`). |
| d3d9 `ff_lighting.h` | `ffl_normal_matrix`: a world(-view) matrix with a fourth column other than (0, 0, 0, 1) takes the normals through the 3 x 3 of its whole 4 x 4 inverse (Windows: ddraw7 `test_lighting` "non-affine" is black); `ffl_normal_matrix3` is the 3 x 3 alone, Direct3D 1-3's. |
| user32 `display.inc` | EnumDisplaySettingsA/ExA, ChangeDisplaySettingsA/ExA, EnumDisplayDevicesA; ChangeDisplaySettingsExW sends WM_DISPLAYCHANGE to the top-level windows. |
| ole32 `compobj.inc` | CLSID_DirectDraw, CLSID_DirectDraw7, CLSID_DirectDrawClipper are ddraw.dll's classes. |
| d3d9 `resources.inc`, `formats.inc` | locks of D16_LOCKABLE / D32F_LOCKABLE / D32_LOCKABLE depth buffers read the depth from the device and write it back (`tex_depth_copy`; they had a pitch of 0 and no contents) - what Lock of a DirectDraw z buffer after Direct3D drew needs. A depth buffer that goes takes the clears nothing drew after with it before the list is sent (the kernel refused the whole list otherwise). |
| d3d9 `draw.inc` | D3DSHADE_FLAT in the fixed-function vertex stage: each primitive's vertices copied with the colour and specular colour of its first vertex (a fan's second) and drawn as a list - there is no provoking-vertex state below (Wine's test_shademode). |
| d3d9 `ff_stages.h`, `d3d9.c` | stage state 33, `TCOS_TSS_KEY_KILL` (d3d9.dll's own, which ddraw.dll sets): a texel of alpha 0 of the stage's texture takes the pixel away (texkill) before the stages and the alpha test - DirectDraw's colour key. `tests/FfStagesTest.cpp` checks it. |
| `SystemDxg.cpp` | the CPU renderer's adapter (the standard VGA card) gets the default 256 MB video memory budget instead of the framebuffer card's 3 MB: a 640x480 target with a z buffer and its copies were evicted, and the draws into them lost. |
| `SoftwareDevice.cpp` | window positions are snapped to 1/256 pixel (Direct3D's 16.8 fixed point, as GPUs do): a vertex a rounding error past a pixel centre (Direct3D 9's whole-number centres moved by half a pixel through the viewport) no longer takes that pixel (Wine's test_viewport). |

## The Direct3D part (hooks)

The Direct3D interfaces are written beside this code in the same library ("Direct3D 1, 2, 3 and 7" below). They plug in by defining

```c
void dd_d3d_register(DdD3dHooks *hooks);   /* (weak in ddraw.c: called from DllMain before anything is made) */
```

and filling the fields they have; a field left NULL means that part of Direct3D does not exist (QueryInterface answers E_NOINTERFACE):

| hook | |
|---|---|
| `d3d7_vtbl`, `d3d3_vtbl`, `d3d2_vtbl`, `d3d1_vtbl` | the vtables of IDirect3D7/3/2/IDirect3D. Every DirectDraw object has the fields `vt_d3d7` .. `vt_d3d1` (interface pointers: `&dd->vt_d3d7`, recover the object with `DD_IMPL(DDraw, vt_d3d7, iface)`); QueryInterface hands them out (and sets `dd->d3d_version`). Their AddRef/Release must count on IDirectDraw7 (IDirect3D7) and IDirectDraw (the others), as Windows. |
| `texture2_vtbl`, `texture1_vtbl` | IDirect3DTexture2 / IDirect3DTexture on surfaces (`s->vt_tex2`, `s->vt_tex1`; QueryInterface of version 1-4 surfaces hands them out with a reference of `s->texture_outer`, the creation version's interface, which their AddRef/Release must use). |
| `ddraw_query(dd, riid, out)` | QueryInterface of a DirectDraw object for anything ddraw does not know. |
| `surface_query(s, riid, out)` | QueryInterface of a version 1-4 surface for what it does not know (a Direct3D device by `IID_IDirect3DHALDevice` etc.). E_NOINTERFACE: ddraw answers E_INVALIDARG as Windows. |
| `pull(s, rect)` | called before the CPU reads or writes a surface's pixels (Lock, GetDC, Blt/BltFast source and destination, the presentation of the primary) when `s->store.shadow.state & DD_SHADOW_GPU_NEWER`: copy the Direct3D 9 copy into `s->store.mem` (`s->desc.lPitch`, `s->fmt`) and clear the flag. |
| `cpu_wrote(s, rect)` | called after the CPU wrote a surface (Unlock of a writing lock, ReleaseDC, Blt/BltFast/fills into it, SetSurfaceDesc, Restore, SetColorKey, SetPalette, a palette's SetEntries, SetLOD); `DD_SHADOW_CPU_NEWER` is set before. |
| `flipped(chain, n)` | the stores of `n` surfaces of a flipping chain went round (the shadows with them). |
| `surface_destroyed(s)`, `ddraw_destroyed(dd)` | free what the Direct3D part keeps: `s->store.shadow.resource`, `s->d3d`, `dd->d3d`. |

`SurfaceStore` (`s->store`: `mem`, `size`, `shadow {resource, state}`) is what Flip exchanges. The pixel code the Direct3D part can call:
`surface_image(s, &image, palette)` (a `DdImage` of a surface, palette as A8R8G8B8), `dd_to_argb(&image, &rect, out, size, pitch, key, low, high)`
(a rectangle as A8R8G8B8 with the colour key as alpha 0: texture uploads of keyed or palette textures), `dd_blit`, `dd_fill`, `dd_read_argb`,
`dd_write_argb`, `dd_depth_to_u32`/`dd_depth_from_u32`, and the surface fields `desc`, `fmt`, `palette`, `complex[]`, `next_attached` (the
attached z buffer), `lod`, `priority`, `version`, `dd`. Take `dd_lock()` (the global lock, re-entrant) around anything that walks or changes them.

## Direct3D 1, 2, 3 and 7

The Direct3D part of ddraw.dll (`d3d*.inc`, included at the end of `ddraw.c`) fills the hooks above. It is built on d3d9.dll's COM interfaces, which
it loads when Direct3D is first used (LoadLibrary: a program that only uses DirectDraw never loads it). It loads the System32 `d3d9.dll` by its path
(`GetSystemDirectoryW` + `\d3d9.dll`, SysWOW64 for a 32-bit program through the loader), never a `d3d9.dll` the program brought or loaded (DXVK, a
ReShade proxy): Windows' ddraw.dll does not use Direct3D 9, and this one needs d3d9's private interfaces (WindowsUserMode.md, "Libraries of the same
name"; `tests/bundled_dlls_qemu.sh`). `d3d9_ddraw.h` declares them for a
translation unit that has mingw's `d3d.h` (`scripts/gen-d3d9-ddraw-abi.py`: the enumerations both headers name are DWORDs, the structures of the same
layout are Direct3D 7's). The logic without COM objects is `d3d_legacy.h` ([Direct3DLegacy.md](Direct3DLegacy.md)).

| file | what |
|---|---|
| `d3d.inc` | the objects (`D3dDD` per DirectDraw object, `D3dDevice`, viewports, lights, materials, execute and vertex buffers, `D3dRes` copies of surfaces), the handle tables, the FPU control word, the Direct3D 9 device, the copies of surfaces (targets, depth buffers, textures) and the DirectDraw hooks |
| `d3d_state.inc` | the states of a device (what the program set, kept for Get*), dirty masks, state blocks, what goes to Direct3D 9 before a draw (`d3_flush`), the CPU lighting and transform of a device |
| `d3d_draw.inc` | drawing: Direct3D 7 (and transformed vertices of every version) through Direct3D 9's fixed function, Direct3D 1-3 transformed, lit and clipped on the CPU; the clip status; Clear |
| `d3d_device.inc` | device creation and references, the methods the versions share (caps, render/light states, transforms, viewports of a device, Begin .. End, texture stages, strided and vertex buffer draws) |
| `d3d_methods.inc` | the methods of IDirect3DDevice7, 3, 2 and IDirect3DDevice (state blocks, Load, matrices, execute, pick ...) |
| `d3d_objects.inc` | IDirect3DLight, IDirect3DMaterial/2/3, IDirect3DViewport/2/3, IDirect3DExecuteBuffer (and running one), IDirect3DVertexBuffer/7, IDirect3DTexture/2 |
| `d3d_vtbl.inc` | IDirect3D, 2, 3 and 7, the device vtables, `dd_d3d_register` |

| part | how |
|---|---|
| Direct3D 9 device | one per DirectDraw object (`dd->d3d`), made on first use on a hidden window of its own, windowed, an 8x8 back buffer that is never shown, software vertex processing, FPU preserved. Every Direct3D device of the object draws through it: the device whose states it has is `active`; another one puts all of its states back (`all_dirty`) before it draws. |
| surfaces | the DirectDraw memory stays what counts. A surface Direct3D draws into or samples gets a Direct3D 9 copy, `D3dRes`, in `s->store.shadow.resource` (Flip moves it with the memory): a render target X8R8G8B8 / A8R8G8B8 (whatever the DirectDraw format: converted both ways, 5-6-5 and 5-5-5 with fast paths) with a system-memory twin for GetRenderTargetData / UpdateSurface; a depth buffer D32F_LOCKABLE (D24S8 when the format has stencil bits, made lockable with d3d9.dll's own usage `TCOS_USAGE_LOCKABLE_DEPTH` through `CreateDepthStencilSurfaceEx`: depth codes in its low 24 bits, the stencil above, converted to the DirectDraw format's masks both ways); a texture A8R8G8B8 / X8R8G8B8 of the managed pool with every level or cube face; a texture with DDSCAPS_3DDEVICE is a render target texture. A CPU write (`cpu_wrote`) of a target or depth buffer is sent at once - a rectangle of one value as a Clear, the rest through UpdateSurface or a lock of the lockable depth -; a texture is sent at its next use (each level's DD_SHADOW_CPU_NEWER). A draw marks the target (and a written depth buffer) DD_SHADOW_GPU_NEWER and `pull` reads it back only when the CPU touches it (Lock, GetDC, Blt, the primary on the screen). |
| textures | every DirectDraw format is converted on the CPU (`dd_to_argb`: palettes, DXT, YUV, luminance). Colour keying (D3DRENDERSTATE_COLORKEYENABLE with a DDSD_CKSRCBLT texture): the key is compared with the texel's colour and alpha bits (Windows does: Wine's test_ck_rgba); keyed texels become alpha 0, the others at least 1/255 (a keyed texture of a format without alpha still counts as one without, so MODULATE takes the vertex's alpha: test_texturemapblend), and d3d9's `TCOS_TSS_KEY_KILL` on the stage discards alpha-0 texels whatever the stages then make of the alpha (MODULATEALPHA with a diffuse alpha of 0 still shows the unkeyed texels: test_colorkey_precision). A palette change, SetColorKey, Load and the CPU's writes make the copy again; SetLOD is the managed texture's LOD. |
| Direct3D 7 | draws with Direct3D 9's fixed function: render states through `dl_render_state`, texture stage states (the render states of stage 0's sampler are stage 0's states, as they share them), transforms, materials, clip planes, lights (the first eight enabled lights by index are Direct3D 9's lights 0-7), vertex declarations from the FVF code (`dl_fvf_elements`: D3DFVF_RESERVED1 is not d3d9's PSIZE). The clip status is worked out on the CPU from the positions. ProcessVertices is the CPU's (`dl_process_fvf` with Direct3D 7 lighting; the specular alpha is that of the specular colour's source, and a singular world matrix lights no normal - Windows' answer there, while its draws use the matrix itself). Direct3D 3's ProcessVertices gives lit vertices a specular alpha of 0. DXTn surfaces must be whole 4 x 4 blocks (DDERR_INVALIDPARAMS otherwise). |
| Direct3D 1-3 | untransformed vertices are transformed with the current viewport's clip space, lit with its lights (lights of no colour are skipped: they add nothing and would take places - test_specular_lighting adds 64 of them) (the legacy model, D3DLIGHT/D3DLIGHT2) and the material of D3DLIGHTSTATE_MATERIAL, clipped (Sutherland-Hodgman, `dl_clip_polygon`) and drawn as transformed vertices; transformed vertices go to Direct3D 9 as they are - with the z test off, a copy with z clamped to [0, 1] when one is outside (Windows does not clip them by depth then: test_zenable). TEXTUREMAPBLEND drives stage 0 (`dl_texture_map_blend`, MODULATE taking the texture's alpha only if it has one) until a texture stage state changes. |
| execute buffers | `dl_execute_*` walks the instructions; PROCESSVERTICES fills the device's D3DTLVERTEX array and their clip-space copies, POINT/LINE/TRIANGLE/SPAN draw from them (clipped; wireframe edges by the triangle flags), MATRIXLOAD/MULTIPLY on matrix handles, STATETRANSFORM (SetMatrix of a matrix a transform came from changes the transform), STATELIGHT, STATERENDER, TEXTURELOAD, the status for BRANCHFORWARD and SETSTATUS; ZVISIBLE through an occlusion query (D3DSTATUS_ZNOTVISIBLE cleared when something passed). Pick finds the triangles under the rectangle's corner. Execute of a locked buffer D3DERR_EXECUTE_LOCKED. |
| references | IDirect3D7 counts on IDirectDraw7, the others on IDirectDraw; a device holds its target's interface (not Direct3D 1: QueryInterface of a surface makes the device aggregated into the surface, counting on its IDirectDrawSurface, destroyed with it); a vertex buffer of IDirect3D7 holds IDirectDraw7; viewports added to a device are held by it; SetTexture holds nothing (a texture that goes is unbound). SetRenderTarget follows Wine's tests of Windows (a depth buffer is taken and refused, a system-memory target of a hardware device is taken and ignored by 2 and 3) and leaves D3DRENDERSTATE_ZENABLE as the program set it. |
| handles | materials, textures and matrices in one table of the process (Windows' handles may be used with any device), state blocks per device. |
| FPU | without DDSCL_FPUPRESERVE device creation sets the x87 control word to single precision with exceptions masked; with it Direct3D 7's methods set it up and put the program's back (Wine keeps two vtables for this). |
| presentation | Flip of a 3D flipping chain shows the rendered frame (the front buffer is pulled for the screen); a scene drawn straight into the primary is shown at EndScene. |

What is not there, and why:

- Bump maps (V8U8, L6V5U5, X8L8V8U8, Q8W8V8U8, V16U16) go to d3d9.dll as its signed formats, their pixels as they are; D3DTOP_BUMPENVMAP and
  BUMPENVMAPLUMINANCE are d3d9.dll's fixed function's (docs/Direct3D9.md).
- A D3DVIEWPORT7 whose depth range is less than 0.001 (dvMinZ and dvMaxZ left 0, or turned round) goes to Direct3D 9 as 0.001 from dvMinZ on, as
  Windows draws it (Wine's `test_viewport`).
- A depth buffer of any format but D24FS8 is lockable in Direct3D 9 (`TCOS_USAGE_LOCKABLE_DEPTH`): Lock reads what Direct3D drew, stencil included,
  and Unlock writes it back.
- EnumDevices hands out the same writable name and description strings every time (`d3d_device_strings`), which keep what a program wrote into them.
  EnumZBufferFormats lists 16, 24 and 32-bit formats Windows lists (no D15S1 or D24X4S4: CreateSurface takes them). An object DirectDrawCreateEx made
  gives only IDirect3D7, one of DirectDrawCreate every IDirect3D but 7 (Wine's refcount test).
- Ramp (palette) devices draw in RGB, as DirectX 6 and later do; line patterns, stippling, ROP2 and plane masks are kept as states and not drawn (the
  caps do not claim them).
- IDirect3DExecuteBuffer::Validate and Optimize, IDirect3DMaterial::Reserve/Unreserve, IDirect3DViewport::LightElements: DDERR_UNSUPPORTED, which is what
  Windows' runtime answers (Microsoft documents them as not implemented).
- GetInfo: S_FALSE, the retail runtime's answer (statistics are a debug runtime's).
- A device draws on the CPU path for 1-3 and the fixed function for 7, so two devices of different versions on one object switch all states each time
  they alternate (correct, slower).
- ProcessVertices of Direct3D 7 as Windows': lights vertices without a normal (ambient and emissive only); gives a vertex without a specular
  colour a specular alpha of 0 unless lighting or fog writes it; keeps the last material power above 0 when the material's is 0; under a world matrix
  that is not affine takes the normals through the 3 x 3 of the whole inverse (as Direct3D 9's draws, `ffl_normal_matrix`; Direct3D 1-3 the 3 x 3
  alone); blends homogeneous positions (a w is kept, Windows' rhw). Execute buffers: the vertices an instruction may name are those the buffer has
  room for after dwVertexOffset, dwVertexCount does not gate them. Material handles are one per device (`test_multiple_devices`); a surface without
  DDSCAPS_TEXTURE set as a texture in Direct3D 1-3 is kept (GetTexture) and drawn by the software devices only (`test_texture_wrong_caps`);
  IDirect3DDevice2 keeps the reference SetCurrentViewport took for good (Windows' leak, `test_viewport_object`).
- Found by Wine's tests and not fixed yet (numbers in `tests/windows/wine/README.md`): user clip planes (Direct3D 9 here has
  none: SetClipPlane is DDERR_INVALIDPARAMS, `test_compute_sphere_visibility`), DESTALPHA blending on an X8R8G8B8 target and fog with start = end
  (`test_fog_special`) in Direct3D 9's renderer, a depth range beyond [0, 1] is cut to [0, 1] for Direct3D 9 (depths inside it scale differently
  from Windows'), the managed cube map of visual.c's cubemap_test samples as no texture. `test_colorkey_precision` is correct as far as it gets but
  runs past the 40 s limit of the split runs on the CPU renderer.

## Not done, and why

- Overlays and video ports: no overlay plane in the kernel's display; Windows' answers for hardware without them (DDERR_NOOVERLAYHW, DDERR_NOTAOVERLAYSURFACE).
- Colour controls (IDirectDrawColorControl): no driver colour controls; E_NOINTERFACE as Windows without them.
- The windowed primary does not see what other programs draw on the screen after it was made (no read-back of the composited desktop on Lock).
- GDI's own view of a low-colour mode: the kernel reports the depth a program set (EnumDisplaySettings), but GDI keeps drawing 32 bits (GetDeviceCaps(BITSPIXEL) stays 32).
- Range colour keys (DDCKEY_COLORSPACE with low != high): DDERR_NOCOLORKEYHW, as the caps report no range keys (the blitter itself takes ranges).
- The window messages around a focus loss and its return (`test_coop_level_mode_set`): the order of WM_ACTIVATE, WM_ACTIVATEAPP and the mode change
  is Windows' now (win32k's foreground change sends the old window's WM_NCACTIVATE/WM_ACTIVATE before WM_ACTIVATEAPP), but the window manager
  drops the focus when the notes are made, so WM_KILLFOCUS comes after ddraw's minimizing instead of before it; and ShowWindow(SW_RESTORE) of the
  minimized window activates it in its thread without taking the foreground back from the other process (no foreground-permission rules here),
  so ddraw never hears WM_ACTIVATEAPP (TRUE) and does not put the program's mode back.
- Windows' IDirectDrawSurface::DeleteAttachedSurface sometimes leaves a surface of the chain an extra reference (`test_flip`, `test_surface_attachment`
  of ddraw1, which Wine leaves todo); when is not clear from the tests (a plain rule broke other checks), so it is not imitated.
- On 64-bit Windows IDirectDraw7 refuses textures (E_NOINTERFACE: it has no Direct3D); here 64-bit programs keep Direct3D 7, as Wine
  (`test_texturemanage`, `test_lockrect_invalid` and others of ddraw7 count it).

## Tests

- `tests/run_host_tests.sh DdrawPixelsTest` (ASan, UBSan): the formats, conversions, blitter (DXT encoding and partial-block blits too), truncated images of every format, random blits.
- `tests/windows/ddrawtest.c`: `TMPDIR=... tests/wow64_qemu.sh ddrawtest` (32-bit) and `GPU=std tests/d3d12_qemu.sh ddrawtest` (64-bit).
- Wine's tests (`tests/windows/wine/ddraw`): `scripts/winetest.sh ddraw` from a short path (a symlink to the checkout, QEMU's socket path); results in
  `tests/windows/wine/README.md`. A test that hangs stops the rest of its program: run one test function per process with a split copy whose
  START_TEST runs only the function `DDRAW_ONLY` names (generated, not committed) and `WINETEST_QUIET=200 WINETEST_ENV=WINERUN_TIMEOUT_MS=40000`.
- Numbers (2026-11-26 gap pass, 64-bit, CPU renderer, DirectDraw and Direct3D): 411 test functions run, 383 pass; failed checks ddraw1 9, ddraw2 12,
  ddraw4 88, ddraw7 262, d3d 4, dsurface 0, refcount 0, visual 5 (first pass: 316 pass; ddraw1 47, ddraw2 55, ddraw4 163, ddraw7 428, d3d 24, dsurface
  82, refcount 12, visual 10) (per test in `tests/windows/wine/README.md`). `test_map_synchronisation` is left out of the split runs (its draw sizes come from how fast the
  first draw was: minutes on the CPU renderer). `tests/windows/ddrawtest.c` 101 checks and `tests/windows/d3dimtest.c` 89 pass on both widths and on virgl.
- `tests/windows/d3dimtest.c`: Direct3D 1, 2, 3 and 7 - devices, Clear, draws read back, z buffers, colour-keyed textures, lights, state blocks, vertex
  buffers, a 640x480 target with a 24-bit z buffer, a 3D flipping chain, execute buffers, clipping. `GPU=std tests/d3d12_qemu.sh d3dimtest`, the same
  with the default virgl, and `tests/wow64_qemu.sh d3dimtest`.
