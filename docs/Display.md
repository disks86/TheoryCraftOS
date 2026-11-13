# Display scale (HiDPI)

Display Settings gives each monitor a scale from 100 % to 400 % (`hardware::SystemMonitors::set_scale`). The desktop's layout is in **units**: a monitor's
rectangle is its pixels divided by its scale (`MonitorLayout`), and the desktop, its windows, input and every program's coordinates are in units.

## How a scaled monitor is drawn

- **Device scale.** `SystemMonitors::render_scale()` is the largest whole scale of the enabled monitors (200 %: 2, 300 %: 3, up to 4), else 1. The desktop
  draws that many device pixels per unit: the display device's renderer is that large (`MonitorDisplay::update(..., render_scale)`), and the desktop holds a
  `graphics::ScaledRenderer2D` over it (`src/Graphics/Graphics2D/ScaledRenderer2D.hpp`, set up by `DisplayPipeline` in `main.cpp`), whose extent is the
  layout's. The desktop's own drawing is enlarged exactly through it: rectangles, lines and shapes in whole device pixels, the 8x16 font as blocks, images
  and masks by nearest sampling. So it looks as it did at that scale before, while what is drawn at the device's resolution is sharp.
- **Monitors.** `MonitorDisplay` maps the device-scale picture onto each monitor: a monitor at the device scale gets a 1:1 copy; one at another scale is
  resampled (nearest), e.g. a 100 % monitor beside a 200 % one shows every other pixel, a 150 % monitor with no whole-scale monitor beside it the 1x
  picture stretched as before.
- **Programs.** Sessions are told the device scale (`IWindowsSession::Monitor::scale`). A window is drawn with `draw_window_scaled` on the device renderer
  (`IRenderer2D::device()`, `pixel_scale()`) when its session can: the Wayland compositor draws a buffer made at that scale 1:1 and announces the scale
  (`wl_output.scale`, `preferred_buffer_scale`, fractional `preferred_scale`; see [Wayland.md](Wayland.md)). Windows programs are DPI-virtualized as
  on Windows (below): an aware program's window is drawn 1:1, an unaware one's enlarged. X programs' windows are drawn with `draw_window` through the
  scaling renderer, enlarged as everything was before.

## Windows programs: DPI virtualization

win32k works in device pixels (`src/Process/Win32k/Win32kUserDpi.cpp`). `DesktopBridge` multiplies what the desktop gives it by the device scale:
the screen, the monitors and work areas, the desktop's frame, window moves, the mouse, touches and drops. It divides what it reports: window and
surface rectangles, and the cursor. It also sets the system DPI to 96 x the scale.

- A window remembers the DPI awareness of the thread that made it (`Window::dpi_virtual`). That is the thread's or process's context, set with
  `SetProcessDpiAwarenessContext` / `SetThreadDpiAwarenessContext`.
- **Aware** programs (system or per-monitor) see device pixels everywhere and 96 x scale DPI (`GetDpiForWindow`, `GetDeviceCaps(LOGPIXELS)`, the
  system metrics). Their window surfaces are in device pixels, and the desktop draws them 1:1 (`draw_window_scaled`): sharp.
- **Unaware** programs (the default) see 96-dpi units: every coordinate crossing between them and win32k is divided by the scale (floor) or
  multiplied. What such a program sets, it reads back unchanged. This covers:
  - **Calls:**
    - Windows: `CreateWindowEx`, `SetWindowPos` / `MoveWindow` / `DeferWindowPos`, `Get/SetWindowPlacement`, `GetWindowRect` / `GetClientRect` /
      `GetWindowInfo`.
    - Points: `ClientToScreen` / `ScreenToClient` / `MapWindowPoints`, `WindowFromPoint` / `ChildWindowFromPoint(Ex)`.
    - The cursor: `Get/SetCursorPos`, `Get/ClipCursor`, `GetCursorInfo`, `GetMessagePos`.
    - Painting: `Invalidate/ValidateRect(Rgn)`, `RedrawWindow`, `GetUpdateRect(Rgn)`.
    - The screen: `MonitorFromRect`, `GetMonitorInfo`, the primary and virtual screen, `SPI_GET/SETWORKAREA`, the screen-size system metrics,
      `GetDeviceCaps` sizes.
    - Also `InjectTouchInput`, and DefWindowProc's `WM_NCCALCSIZE` and `WM_NCHITTEST`.
  - **What the kernel makes for its windows:**
    - Mouse and wheel lParams and `MSG.pt`, `WM_POINTER*` and touch information, `DROPFILES.pt`, `WM_MOVE` / `WM_SIZE`.
    - The `WINDOWPOS`, `NCCALCSIZE`, `MINMAXINFO` and `CREATESTRUCT` payloads. A value the program hands back unchanged keeps its exact device value,
      so rounding never moves a window.
  - **Its surface:** the window surface is in its units (the device scale smaller), and so are its DCs, update regions, `BeginPaint` and GPU
    presents. The desktop enlarges the surface, as Windows' DWM does.
- Messages a program posts or sends itself keep the coordinates it put in them. The `WM_SYSCOMMAND` the desktop uses to place a window carries
  device pixels, which go back to the kernel unread.

## Not done

- Fractional scales drawn natively (125 / 150 / 175 % keep the stretched 1x picture; the preferred scale is never fractional).
- One device scale for all monitors; no per-monitor density.
- Windows DPI virtualization (above) has these gaps:
  - No `WM_DPICHANGED` when the scale changes: windows are moved to keep their place in units, but aware programs are not told the new DPI.
  - Mixed-awareness window trees (a child of another awareness than its top-level) are not handled.
  - The screen DC (`GetDC(NULL)`) and `GetRandomRgn` stay in device pixels for every caller.
  - Messages one program sends another with coordinates are not converted between their awarenesses.
  - Window regions are read back as they were set.
- X programs are not told the density (`Xft.dpi`): they are enlarged.
- The desktop's own text is the enlarged 8x16 font, not a font drawn at the device size.
- A GPU device image (`draw_device_image`) cannot be enlarged by the scaling renderer: callers draw it from the CPU's view.

## Tests

| What | How |
| --- | --- |
| The scaling renderer: a scene at 2x and 3x equal to the 1x scene enlarged pixel for pixel (rectangles, outlines, lines, text, images, masks, clips), extent, device, read-back, matrices | `tests/run_host_tests.sh GraphicsTest` (`test_scaled_2d`) |
| `MonitorDisplay` at render scale 2: a 200 % monitor 1:1 in one copy, a 100 % monitor beside it halved, a part of the picture, 300 % at 3/2 with its last columns | `tests/run_host_tests.sh MonitorTest` |
| The desktop at scale 2: the session told 2, a window enlarged, then drawn on the device at twice its place when the program draws at the scale | `tests/run_host_tests.sh GuiTest` (`test_windows_hidpi`) |
| Wayland at scale 2 | `tests/run_host_tests.sh WaylandServerTest` (`test_mapping`) |
| Windows DPI virtualization at scale 2. An unaware program: its rectangles, client size, surface size and drawing, mouse lParam and `MSG.pt`, `GetCursorPos`, DPI and screen metrics, and moves both from the desktop (`WM_SIZE` in units) and from the program. An aware program: device-pixel rectangles, 192 dpi, its mouse, and its surface drawn 1:1. The desktop in units both ways | `tests/run_host_tests.sh Win32kUserTest` (`test_dpi_virtualization`) |
| No regression at scale 1 in a real run: `tests/windows/user32test.c` in QEMU | `scripts/winetest.sh user32 tcos` |
