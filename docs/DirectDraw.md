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
| groups | the surfaces of one CreateSurface (a flipping chain, the levels of a mipmap, the faces of a cube map) are a group: the root's last release destroys the group's structure (its attachments, palette); a sublevel or back buffer the program still holds stays usable until its own last release, then the group's memory goes. |
| identities | a surface pointer a program passes in may be any version's interface (Windows takes them, Wine's tests pass them): `surface_from_any` finds the surface by its vtable. QueryInterface answers the versions, `IDirectDrawGammaControl`, `E_NOINTERFACE` for `IDirectDrawColorControl` (no colour controls), and for unknown interfaces `E_NOINTERFACE` on version 7 surfaces, `E_INVALIDARG` on the older ones, as Windows. `IDirectDraw3` is not an interface (E_NOINTERFACE). |
| initialization | DirectDrawCreate(Ex) objects are initialized; CoCreateInstance's answer DDERR_NOTINITIALIZED until `Initialize` (then DDERR_ALREADYINITIALIZED). |
| cooperative level | Windows' rules (see `ddraw_set_cooperative_level`): NORMAL / EXCLUSIVE / SETFOCUSWINDOW, EXCLUSIVE needs FULLSCREEN and a window, CREATEDEVICEWINDOW (a `DirectDrawDeviceWnd` popup) needs a focus window, a second exclusive object gets DDERR_EXCLUSIVEMODEALREADYSET. Exclusive mode sizes the window to the screen (shown when it is active or in the foreground, topmost when in the foreground; DDSCL_NOWINDOWCHANGES: not at all), clips the cursor, and puts ddraw's window procedure in front of the focus window's: WM_ACTIVATEAPP off minimizes the window, puts the desktop's mode back and loses every surface, on puts the program's mode back. The program's procedure comes back only while ddraw's is still the window's (as Windows). Window changes of a window of another thread are made by a thread of their own (that thread may be waiting for the caller). |
| display modes | user32's EnumDisplaySettings / ChangeDisplaySettingsEx; the kernel lists every size at 32, 16 and 8 bits and keeps the depth a program set (`Win32kUserMiscMonitor.cpp`), while scanning out 32 bits. EnumDisplayModes lists them (unique sizes and depths, with DDEDM_REFRESHRATES also by rate), SetDisplayMode takes 8, 15, 16, 24 and 32 bits, a width or height of 0 is DD_OK and changes nothing (as Windows). A mode change by anyone (ChangeDisplaySettings too) loses the surfaces that can be lost. |
| lost surfaces | primary surfaces, z buffers and video memory surfaces; IsLost, Restore (DDERR_WRONGMODE for a primary of another mode), RestoreAllSurfaces, TestCooperativeLevel. Their memory is kept (a restored surface's contents are formally undefined). |
| memory | every surface's pixels are system memory and what counts. A surface without memory caps goes to "video memory" when its format is one the hardware path takes (not palettes, not 3-3-2, not YUV textures), else to system memory (and can be lost, as Windows' fallback can). Pitches: 8-byte rows, 4 for system memory of a 64-bit process (Windows' numbers); block formats report a linear size. GetAvailableVidMem counts the video memory surfaces. DDSD_LPSURFACE (version 4 on) uses the program's memory; SetSurfaceDesc changes it. DuplicateSurface makes a surface object that shares the memory. |
| formats | P1/P2/P4/P8, R3G3B2, A8R3G3B2, R5G6B5, X1R5G5B5, A1R5G5B5, X4R4G4B4, A4R4G4B4, R8G8B8, X8R8G8B8, A8R8G8B8, X8B8G8R8, A8B8G8R8, A2R10G10B10, A2B10G10R10, A8, L8, A8L8, A4L4, L16, V8U8, L6V5U5, X8L8V8U8, Q8W8V8U8, V16U16, D16, D15S1, D24X8 (both masks), D24S8, D24X4S4, D32, YUY2, UYVY, YV12, NV12, DXT1-5 (GetFourCCCodes lists the FourCCs). |
| Lock | rectangles checked as Windows (DDERR_INVALIDPARAMS, block-aligned for DXT), a second lock or a lock while a DC is out DDERR_SURFACEBUSY, Unlock of an unlocked surface DDERR_NOTLOCKED; any of DDSURFACEDESC/2's sizes, and a version 7 system memory texture takes any size (as Windows). |
| GetDC | as Windows' runtime: `D3DKMTCreateDCFromMemory` makes a GDI memory DC whose DIB is the surface's own memory (top-down, 16-bit formats with bit fields; GetObject reports a positive height and no image size, as Windows), so GDI draws straight into the surface. Formats: RGB of 16, 24 and 32 bits and P8; P8 takes the surface's palette, else the primary's, else (8-bit display mode only) the system palette, and keeps those colours until ReleaseDC (a later SetEntries or SetPalette does not change the DC, as Windows). Other formats DDERR_CANTCREATEDC. GetSurfaceFromDC (the methods and the export) finds the surface. |
| Blt | colour fill (raw value), depth fill (depth bits; stencil kept), SRCCOPY/BLACKNESS/WHITENESS ROPs (others DDERR_NORASTEROPHW, as Windows' blitter), stretch and shrink (point sampling), DDBLTFX mirroring and rotation by 90/180/270, source keys (the surface's or the override), destination keys (versions 4 and 7: the destination's own; versions 1-3: the source's DDCKEY_DESTBLT, which is what Wine's test records for Windows), conversion between all colour formats (YUV and DXT sources decoded; a palette destination takes the nearest entry), depth to depth, blits of a surface onto itself (overlapping parts copied away first), the destination's clipper (each rectangle of its clip list, the source scaled to match). DDBLT_ALPHA*/DDBLT_ZBUFFER* are dropped as Windows' software blitter drops them. BltFast: no clipper (DDERR_BLTFASTCANTCLIP), the source and destination keys. BltBatch: DDERR_UNSUPPORTED - Windows never implemented it. |
| flipping chains | primary, off-screen and texture chains (a chain of textures is made, as Windows; Wine refuses it). On 64-bit Windows IDirectDraw7 has no Direct3D and refuses textures with E_NOINTERFACE: the same here while no Direct3D part is registered (IDirectDraw4 and older make them). |
| Flip | the chain's memory (and Direct3D shadows) go round, or the front and a target exchange theirs; the primary is shown, then Flip waits for 1-4 vertical blanks (DDFLIP_INTERVALn, none with DDFLIP_NOVSYNC). Flipping the primary needs exclusive mode. |
| presentation | present.inc: every write into the primary's front buffer goes to the screen through GDI's SetDIBitsToDevice, converted to 32 bits (8-bit primaries through their palette, 16-bit ones converted), with the gamma ramp. Normal level: screen coordinates, to the screen or into a clipper's window (its client area). Exclusive mode: the window's whole DC (the window covers the screen). A new windowed 32-bit primary starts with the screen's contents. |
| palettes | 2/4/16/256 entries, DDPCAPS_8BITENTRIES, ALLOW256 (else entries 0 and 255 are black and white), ALPHA (only on textures), PRIMARYSURFACE while it is the primary's; SetEntries updates Direct3D's copies and the screen (not a DC already given out). |
| clippers | SetHWnd (the window's visible region, GetRandomRgn), SetClipList (DDERR_CLIPPERISUSINGHWND with a window), GetClipList (DDERR_REGIONTOOSMALL, E_FAIL for a destroyed window), IsClipListChanged (the window's visible rectangle against the last answer). Like Windows', methods check that the pointer is a live clipper (DDERR_INVALIDPARAMS / 0). |
| caps | HAL: blits, fills, stretching, mirroring, rotation, colour keys, clipping, system memory blits, FourCC blits, palettes, flipping, mipmaps, z buffers of 16/24/32 bits, primary gamma, flip intervals, 3D devices when the Direct3D part is in; HEL: the software layer's set (as Wine's test records Windows' HEL). No overlays (DDERR_NOOVERLAYHW, DDERR_NOTAOVERLAYSURFACE), no colour controls, no stereo, no video ports. |
| the rest | GetMonitorFrequency, GetScanLine (DDERR_VERTICALBLANKINPROGRESS in the blank), GetVerticalBlankStatus, WaitForVerticalBlank (BLOCKBEGIN, BLOCKEND; BLOCKBEGINEVENT DDERR_UNSUPPORTED as Windows), StartModeTest/EvaluateMode (each mode set for 15 seconds or until passed/failed, DDERR_TESTFINISHED at the end, the desktop's mode back), Compact (DDERR_NOEXCLUSIVEMODE outside exclusive mode), EnumSurfaces (DOESEXIST all/match/nomatch, CANBECREATED), GetDeviceIdentifier (d3d9.dll's adapter; only up to dwWHQLLevel is written), private data, uniqueness values (they change with every write), PageLock/PageUnlock (counted for system memory surfaces: DDERR_NOTPAGELOCKED), priority and LOD of managed textures. |

## Layers below it that this work changed

| layer | change |
|---|---|
| `Win32kUserMiscMonitor.cpp` | display modes at 32, 16 and 8 bits; ChangeDisplaySettings keeps the depth a program set (scanout stays 32). |
| `Win32kGdiDib.cpp` | `NtGdiDdDDICreateDCFromMemory` / `DestroyDCFromMemory` (a DIB on the program's memory; the colour table from PALETTEENTRYs). |
| `Win32kGdiBlt.cpp`, `Win32kGdiDraw.cpp` | ROPs on a 32 bpp BI_RGB DIB of the program's work on the whole pixel (`rop3_apply_bits`): WHITENESS sets the fourth byte, BLACKNESS and brushes clear it, SRCCOPY from a 32 bpp source copies it, as Windows. Screen and window surfaces keep their own. |
| `Win32kGdiObjects.cpp` | `NtUserSelectPalette` (was not registered: SelectPalette always returned NULL). |
| `Win32kUserInput.cpp` | WM_ACTIVATEAPP to the top-levels of the threads that lose and gain the foreground; a hidden top-level can be made the foreground window (as Windows). |
| user32 `display.inc` | EnumDisplaySettingsA/ExA, ChangeDisplaySettingsA/ExA, EnumDisplayDevicesA; ChangeDisplaySettingsExW sends WM_DISPLAYCHANGE to the top-level windows. |
| ole32 `compobj.inc` | CLSID_DirectDraw, CLSID_DirectDraw7, CLSID_DirectDrawClipper are ddraw.dll's classes. |

## The Direct3D part (hooks)

The Direct3D interfaces are written beside this code in the same library (they are not here yet). They plug in by defining

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

## Not done, and why

- Overlays and video ports: no overlay plane in the kernel's display; Windows' answers for hardware without them (DDERR_NOOVERLAYHW, DDERR_NOTAOVERLAYSURFACE).
- Colour controls (IDirectDrawColorControl): no driver colour controls; E_NOINTERFACE as Windows without them.
- The windowed primary does not see what other programs draw on the screen after it was made (no read-back of the composited desktop on Lock).
- GDI's own view of a low-colour mode: the kernel reports the depth a program set (EnumDisplaySettings), but GDI keeps drawing 32 bits (GetDeviceCaps(BITSPIXEL) stays 32).
- Range colour keys (DDCKEY_COLORSPACE with low != high): DDERR_NOCOLORKEYHW, as the caps report no range keys (the blitter itself takes ranges).
- Direct3D 1 .. 7: the separate part above.
- The order of window messages around a focus loss and mode change (`test_coop_level_mode_set`): Windows sends WM_ACTIVATE, the window changes
  and WM_DISPLAYCHANGE before ddraw reports the loss at WM_ACTIVATEAPP; here WM_ACTIVATEAPP comes first (the window manager's order).
- P8 to RGB blits look the colours up in the palette (as Wine); Windows' drivers copy the index as a grey level (`test_p8_blit`).
- Windows quirks Wine also leaves (`test_surface_attachment` extra references, attaching a surface attached elsewhere).

## Tests

- `tests/run_host_tests.sh DdrawPixelsTest` (ASan, UBSan): the formats, conversions, blitter, truncated images of every format, random blits.
- `tests/windows/ddrawtest.c`: `TMPDIR=... tests/wow64_qemu.sh ddrawtest` (32-bit) and `GPU=std tests/d3d12_qemu.sh ddrawtest` (64-bit).
- Wine's tests (`tests/windows/wine/ddraw`): `scripts/winetest.sh ddraw` from a short path (a symlink to the checkout, QEMU's socket path); results in
  `tests/windows/wine/README.md`. A test that hangs stops the rest of its program: run one test function per process with a split copy whose
  START_TEST runs only the function `DDRAW_ONLY` names (generated, not committed) and `WINETEST_QUIET=200 WINETEST_ENV=WINERUN_TIMEOUT_MS=40000`.
- Numbers (2026-11-26, 64-bit): 402 test functions, 367 pass, 132 failed checks; ddrawtest 101 checks (32-bit) and 102 (64-bit) pass.
