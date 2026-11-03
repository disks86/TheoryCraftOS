# win32k: the GDI module

The GDI module (`src/Process/Win32k/Win32kGdi*.cpp`) is the kernel side of the `NtGdi*` services of `win32u.dll`, except the Direct3D ones (`NtGdiDd*`). It follows
Wine's prototypes in `tests/windows/wine/include/ntgdi.h`. `Win32kGdi.hpp` is the contract with the other modules (screen, window DCs, regions, bitmaps) plus the
font-provider and DIB-memory hooks; `Win32kGdiInternal.hpp` is private to the module.

168 of the 345 services are implemented (counted by `registered_count()` with only this module installed); the rest keep answering `STATUS_NOT_IMPLEMENTED`.

## Files

| File | Contents |
| --- | --- |
| `Win32kGdi.cpp` | the handle table, stock objects, process-exit hook, the screen surface, the contract functions, DIB-section memory, `install_gdi()` |
| `Win32kGdiSurface.cpp` | `Surface` (pixel access for 1/4/8/16/24/32 bpp, bit fields, bottom-up and top-down), ROP2 and ROP3 |
| `Win32kGdiRegion.cpp` | banded region algebra (sweep over bands), polygon/ellipse/round-rect regions, the region services |
| `Win32kGdiObjects.cpp` | pens, brushes, fonts, palettes, bitmaps, colour spaces, SelectObject, DeleteObject, GetObject |
| `Win32kGdiDc.cpp` | device contexts, SaveDC/RestoreDC, attributes, mapping and world transform, clipping, device caps |
| `Win32kGdiDraw.cpp` | `Painter`, patterns and hatches, lines (cosmetic and geometric pens), rectangles, ellipses, arcs, polygons, curves, pixels, flood fill, region painting |
| `Win32kGdiPath.cpp` | BeginPath/EndPath, fill/stroke/widen/flatten, PathToRegion, SelectClipPath, GetPath |
| `Win32kGdiBlt.cpp` | BitBlt, StretchBlt, PatBlt, MaskBlt, PlgBlt, TransparentBlt, AlphaBlend, GradientFill |
| `Win32kGdiDib.cpp` | BITMAPINFO parsing (core/V3/V4/V5, bit fields, RLE), DIB sections, GetDIBits, SetDIBitsToDevice, StretchDIBits, DIB brushes |
| `Win32kGdiText.cpp` | text output and metrics with the built-in font and the font-provider hook |
| `Win32kGdiMisc.cpp` | gamma ramp, colour adjustment, pixel formats, escapes, metafile DC, engine bitmap/palette services |

## Objects and handles

A handle is `generation << 24 | stock << 23 | type << 16 | index` (index 1..65535, never 0). Types: DC `0x01`, region `0x04`, bitmap `0x05`, palette `0x08`,
colour space `0x09`, font `0x0A`, brush `0x10`, client object `0x20`, pen `0x30`, extended pen `0x50`. The generation changes whenever a slot is reused, so a stale handle never
finds a newer object. Stock objects (`stock_object(index)`, GetStockObject indexes 0..19) have generation 1 and the stock bit; deleting one does nothing.

Every object belongs to a process. A process may only use its own objects and the stock ones. When a process ends (`register_process_exit_hook`) its DCs go first
(they drop what they have selected) and then everything else. Deleting an object that is selected into a DC fails (returns FALSE), as on Windows.

GDI shares DC attributes with user mode through a shared memory table on Windows (`PEB->GdiSharedHandleTable`, `DC_ATTR`). This kernel has no such table, so
`GetObjectType`-style queries by gdi32 and direct `DC_ATTR` access do not exist; the kernel keeps the state itself and these services expose it:

* `NtGdiGetAndSetDCDword(hdc, method, value, &old)`: Wine's methods `8` (SetMapMode) and `100..107` (SetBkColor, SetBkMode, SetTextColor, SetDCBrushColor,
  SetDCPenColor, SetGraphicsMode, SetROP2, SetTextAlign), plus these not in Wine's header: `108` SetPolyFillMode, `109` SetStretchBltMode, `110` SetArcDirection,
  `111` SetRelAbs, `112` SetCharExtra, `120..127` window origin x/y, window extent x/y, viewport origin x/y, viewport extent x/y (setting an extent makes the mode MM_ANISOTROPIC).
* `NtGdiGetDCDword(hdc, method, &value)`: Wine's `0..11`, plus `12` stretch mode, `13` rel/abs, `14` char extra, `15` text align, `16` map mode, `20..27` the window/viewport values.
* `NtGdiGetDCPoint(hdc, method, &pt)`: Wine's `0..2` (brush origin, current position, DC origin), plus `3` viewport origin, `4` window origin, `5` viewport extent, `6` window extent.

## Surfaces and DCs

* The screen is one 32-bit surface (B, G, R, X) made on first use at 1024 x 768 (`set_screen_size`). Window DCs (`create_screen_dc`) have an origin and a list of clip
  rectangles in screen coordinates; the clip of the DC (SelectClipRgn, meta region) is intersected with it. Every change calls `IScreenSink::screen_changed` with the
  bounding rectangle of what the call touched (once per call, when the `Painter` ends).
* A memory DC starts with the 1x1 monochrome stock bitmap. Bitmaps are device-dependent (zeroed, default colour table for <= 8 bpp) or DIB sections; DDBs made by
  CreateCompatibleBitmap take the depth of the DC (screen: 32 bpp).
* **DIB sections** live in the memory of the calling process (so the program can write the pixels directly): the module maps a region in the process's address space
  (`Process::address_space()`, `find_free` + `map`), and unmaps it when the bitmap goes. The kernel keeps a mirror (`Bitmap::data`); `Painter` reads the process's memory into
  it before drawing and writes it back afterwards (a whole-image copy per drawing call: correct and simple, not fast). A host with no address space (the unit tests)
  installs `set_dib_memory(&hooks)`; without a hook and without an address space CreateDIBSection fails with `ERROR_NOT_ENOUGH_MEMORY`. A DIB section with a file-mapping
  section handle is refused (`ERROR_NOT_SUPPORTED`).
* GetPixel/SetPixel outside the clip answer `CLR_INVALID`; every pixel write is clipped and bounds-checked by `Surface::put`, no pixel loop can leave a surface.
* Drawing on information DCs (`CreateMetafileDC`, printer DCs from `OpenDCW`) draws nothing: they have no surface.

## What is faithful and what is approximate

* Regions, ROP2/ROP3, mapping modes, the world transform (GM_ADVANCED), SaveDC/RestoreDC, clip/meta regions, paths (including beziers), DIB formats and bit fields
  follow Windows. Region operations produce canonical banded rectangles; `GetRegionData` returns `RDH_RECTANGLES`.
* Lines exclude the end pixel; a 1-pixel pen frames a rectangle on `[l, r-1]`; a NULL pen fills the whole rectangle. Cosmetic dash patterns are 18/6, 3/3, 9/6/3/6 and
  9/3/3/3/3/3 pixels. Wide pens are drawn as a union of polygons (round, square or flat caps; round, bevel or miter joins); dashes on geometric pens are drawn solid, and
  extended pens use their colour only (hatch/pattern pen brushes are not applied).
* Arcs, pies, chords, round rectangles and rotated ellipses are polygons through sampled points; axis-aligned ellipses and round rectangles are filled exactly from
  scanlines. Not pixel-identical to Windows.
* StretchBlt, AlphaBlend and PlgBlt sample the nearest pixel (all stretch modes behave like COLORONCOLOR). A monochrome source is shown in the destination's text and
  background colours. A DC's bounds rectangle (SetBoundsRect) is stored but drawing does not accumulate into it.
* Layout (RTL mirroring), ICM and colour spaces are stored and returned but have no effect on pixels. Palettes work as colour tables for <= 8 bpp bitmaps and
  PALETTEINDEX/DIBINDEX colours; there is no system-palette realisation.

## Text

Text is drawn with `hardware::Font8x16` (printable ASCII; other characters show `?`), scaled by nearest neighbour to the requested cell height (`|lfHeight|`, default 16) and
width (`lfWidth`, default half the height); bold smears one pixel, italic shears, underline and strike-out are lines. Rotation (`lfEscapement`) is ignored. A table of built-in face names
(System, Terminal, Courier New, Arial, Segoe UI, ...) all map to this font; unknown names show as `System`. `GetTextMetricsW`, `GetTextExtentExW`, `GetCharWidthW`,
`GetCharABCWidthsW`, `GetGlyphIndicesW`, `GetTextFaceW`, `EnumFonts` work from it.

`IGdiFontProvider` (in `Win32kGdi.hpp`, `set_font_provider`) lets the kernel supply real fonts: `match` a request to a face, give `metrics` for a pixel height and 8-bit
coverage `glyph`s (and optionally `font_data` for GetFontData). When a provider is set, GDI asks it first for every selected font and anti-aliases its glyphs; the built-in
font is the fallback. A provider backed by `src/Font` (TrueType) is not written yet.

## Limits and hardening

* All user pointers are read and written through `Call` (bounds-checked, fail instead of fault); counts from user mode are capped (points 1M, text 64K characters,
  16 dash entries, saved DC levels 8192, handles 65535); bitmap and DIB sizes are capped at 32768 x 32768 and 256 MiB.
* A drawing call has a work budget for line drawing (2^27 pixels); wide strokes are limited to 2048 vertices; polygon fills to 2e8 edge-scanline tests; long segments are
  clipped to the clip rectangle before they are walked.
* Size arithmetic uses 64 bit and `sat_add/sat_sub` for coordinates; the screen DC, memory DC and DIB paths never index outside `Surface::bytes`.

## Services not implemented, and why

* **Engine callbacks for kernel display drivers** (`EngBitBlt`, `EngCopyBits`, `EngLineTo`, `EngTextOut`, `EngPaint`, `EngStretchBlt*`, `EngTransparentBlt`, `EngGradientFill`,
  `EngAlphaBlend`, `EngPlgBlt`, `EngFill/Stroke/StrokeAndFillPath`, `EngLockSurface`, `EngUnlockSurface`, `EngAssociateSurface`, `EngCreateDeviceBitmap/Surface`, `EngCreateClip`,
  `EngDeleteClip`, `EngDeletePath`, `EngEraseSurface`, `EngCheckAbort`, `EngMarkBandingSurface`, `EngComputeGlyphSet`) and the **driver object accessors** (`BRUSHOBJ_*`,
  `CLIPOBJ_*`, `FONTOBJ_*`, `PATHOBJ_*`, `STROBJ_*`, `XFORMOBJ_*`, `XLATEOBJ_*`, `HT_*`): they operate on `SURFOBJ`/`CLIPOBJ`/... structures that only exist inside a display
  or printer driver, and there is no driver loader. `EngCreateBitmap`, `EngDeleteSurface`, `EngCreatePalette` and `EngDeletePalette` (the ones that only make or free GDI objects) are implemented.
* **Printing and spooling**: `StartDoc`, `EndDoc`, `AbortDoc`, `StartPage`, `EndPage`, `DrawEscape`, `DoBanding`, `GetPerBandInfo`, `GetSpoolMessage`, `InitSpool`, `UnloadPrinterDriver`,
  UMPD (`SetUMPDSandboxState`, `DisableUMPDSandboxing`, `SetPUMPDOBJ`, `UMPDEngFreeUserMem`), `ConvertMetafileRect`, `CreateServerMetaFile`, `GetServerMetaFileBits`: they need
  the print service (`src/SystemPrintService.cpp`); a state machine that only answered "ok" would make programs believe they printed. `ExtEscape` answers "not supported" like a display driver.
* **Font management**: `AddFontResourceW`, `RemoveFontResourceW`, `AddFontMemResourceEx`, `RemoveFontMemResourceEx`, `AddRemoteFontToDC`, `AddEmbFontToDC`, `GetEmbedFonts`, `MakeFontDir`,
  `GetFontFileData`, `GetFontFileInfo`, `GetFontResourceInfoInternalW`, `GetUFI*`, `SetLinkedUFIs`, `GetLinkedUFIs`, `UnmapMemFont`, `QueryFonts`, `QueryFontAssocInfo`, `GetETM`, `GetWidthTable`,
  `GetGlyphOutline`, `GetCharacterPlacementW`, `GetStringBitmapW`, `SetFontXform`, `ForceUFIMapping`, `ChangeGhostFont`, `WaitForTextReady`, ...: they need a font file loader and a real
  font provider behind `IGdiFontProvider`.
* **Monitor protection and DDC/CI** (`*OPM*`, `DDCCI*`, `GetCertificate*`, `*PhysicalMonitor*`, `GetMonitorID`, `GetCOPPCompatibleOPMInformation`): belong with the display/monitor
  stack, not with the pixel engine.
* **Colour management objects** (`CreateColorTransform`, `DeleteColorTransform`, `CheckBitmapBits`, `ColorCorrectPalette`, `IcmBrushInfo`, `SetIcmMode`, `GetColorSpaceforBitmap`) and
  `SwapBuffers`, `GetDeviceCapsAll`, `GetDeviceWidth`, `GetStats`, `GetEntry`, `GetDhpdev`, `Init2`, `DrawStream`, `EnumObjects`, `GetObjectBitmapHandle`, `MakeObjectXferable`,
  `MakeObjectUnXferable`, `ScaleRgn`, `ScaleValues`, `UpdateTransform`, `SetMagicColors`, `SetPrivateDeviceGammaRamp`, `GetGammaRampCapability`, `Begin/EndGdiRendering`, `DwmCreatedBitmapRemotingOutput`,
  `CreateBitmapFromDxSurface*`, `CreateSessionMappedDIBSection`, `HLSurface*`, `FullscreenControl`, `EnableEudc`, `EudcLoadUnloadLink`, `GetEudcTimeStampEx`: undocumented or tied to
  components that do not exist here (DWM, EUDC, OpenGL ICD, session-mapped memory); implementing them as stubs would invent behaviour.

## Contract notes for the other modules

* `shutdown()` drops all GDI state; the unit tests of every win32k module should call it (after `process_exited(pid)`) before their `System` goes away, because the module
  state is global. Without it the state is dropped silently the next time a different allocator shows up (no free: its allocator is gone).
* `bitmap_pixels` answers alpha 0xFF for a 32-bit bitmap unless some pixel has a non-zero alpha byte; `create_bitmap` keeps the alpha it is given.
* `delete_dc` also works on DCs made by `NtGdiCreateCompatibleDC`/`OpenDCW`. Window DCs made by `create_screen_dc` belong to the pid given, so the process that
  owns the window can use the handle in all `NtGdi*` calls.

## Tests

`tests/run_host_tests.sh Win32kGdiTest` (ASan + UBSan): stock and created objects, handle ownership, DC state and mapping, pixels and shapes, blits, regions (including random
rectangle sets checked pixel by pixel), clipping, paths, DIB formats and conversions, text, the font provider, the screen DC contract with the sink, bad pointers, truncated and
bit-flipped headers, hostile coordinates and polylines, and the process-exit hook.
