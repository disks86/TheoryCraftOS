# Display scale (HiDPI)

Display Settings gives each monitor a scale from 100 % to 400 %, in steps of 25 % (`hardware::SystemMonitors::set_scale`). The desktop's layout is in **units**: a monitor's
rectangle is its pixels divided by its scale (`MonitorLayout`), and the desktop, its windows, input and every program's coordinates are in units.

## How a scaled monitor is drawn

- **Device scale.** `SystemMonitors::render_percent()` is the largest scale of the enabled monitors, as a percentage (125, 150, 200, ... up to 400; 100
  when none is scaled). The desktop draws at it.
  - **One mapping.** Units become device pixels through `src/Graphics/DeviceScale.hpp`: up is ceil(v x p / 100), down is floor(v x 100 / p). Every
    part uses it (the renderer, the monitor compositor, the window systems, DPI virtualization), so they agree to the pixel. A value that goes one way
    and back is the value it was, and edges keep their order.
  - **The renderer.** The display device's renderer is that size (`MonitorDisplay::update(..., render_percent)`). The desktop holds a
    `graphics::ScaledRenderer2D` over it (`src/Graphics/Graphics2D/ScaledRenderer2D.hpp`, set up by `DisplayPipeline` in `main.cpp`), whose extent is
    the layout's.
  - **The desktop's own drawing** goes through it. Rectangles, lines, shapes and the 8x16 font's pixels are mapped by their edges, so what touches still
    touches. At a whole scale a unit is exactly k x k pixels; at 150 % units alternate between one and two pixels. Images and masks are sampled nearest
    at a whole scale and filtered at a fractional one. What is drawn at the device's resolution is sharp.
- **Monitors.** `MonitorDisplay` maps the device-scale picture onto each monitor (by the same mapping):
  - A monitor at the device scale gets a 1:1 copy.
  - One at another scale is resampled (nearest), e.g. a 100 % monitor beside a 200 % one shows every other pixel.
- **Programs.** Sessions are told the device scale (`IWindowsSession::Monitor::scale_percent`). A window is drawn with `draw_window_scaled` on the device
  renderer (`IRenderer2D::device()`, `scale_percent()`) when its session can: the Wayland compositor draws a buffer made at that scale 1:1 and announces the scale
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
- **Child windows** follow Windows' DPI hosting behaviour (`Set/GetThreadDpiHostingBehavior`, `GetWindowDpiHostingBehavior`):
  - **Default hosting:** a new child takes the awareness of the window tree it is put in, and `SetParent` of a child into a tree of another
    awareness fails with `ERROR_INVALID_OPERATION`.
  - **Mixed hosting:** a thread that asked for it keeps its own awareness for its children. A child of another awareness than its tree is a mixed
    child, scaled on its own:
    - Its tree draws on a surface of the child's own, in the child's coordinates.
    - Whenever that surface, or the top-level's under it, changes, win32k scales its pixels into the top-level's surface where the child shows
      (nearest sampling: an unaware child of an aware window is enlarged, an aware child of an unaware window shrunk).
    - Its messages and calls are in its own coordinates.
    - GPU presents to it (or to a window in its tree) go on its own surface, in its units, and are scaled like its drawing. Its window region, in its
      units, shapes the scaled pixels.
- **A scale change** keeps every top-level window where it was in units:
  - A per-monitor-aware window gets `WM_DPICHANGED`, with the new DPI in both halves of wParam and lParam pointing at the suggested rectangle (its old
    one at the new scale), as on Windows. A program that leaves it to DefWindowProc is moved there, which Windows' DefWindowProc does not do. The
    desktop's thread posts a private `WM_SYSCOMMAND` (`kScTcosDpi`), and the window's own thread sends the message.
  - Other windows are moved through the program as a desktop move would be. An unaware window's units stay the same.
  - A system-aware program keeps the system DPI it was made aware at (its awareness context carries it), as on Windows. After the scale grows, its
    windows are virtualized by the ratio of the new DPI to that one, the way unaware windows are: its coordinates, DPI, screen size and surface stay
    what they were, and the desktop draws its pixels enlarged, or shrunk when the scale fell below its DPI (its units are then finer than device
    pixels). A program that becomes system aware after the change works at the new DPI.
- **The screen DC.** For an unaware program on a scaled screen, `GetDC(NULL)` gives a shadow of the screen in the program's units, filled with what the
  screen shows when the DC is made. Whatever the program draws on it goes straight on to the screen, scaled up (each unit pixel over the device pixels it
  stands for). What changes on the screen afterwards is taken into the shadow before the DC is next drawn on or read. The shadow goes away with the DC.
- **`GetRandomRgn` (`SYSRGN`)** is in screen coordinates, in the caller's units, for a window that draws on a window surface as well as for the screen.
- Messages a program posts or sends itself keep the coordinates it put in them. The `WM_SYSCOMMAND` the desktop uses to place a window carries
  device pixels, which go back to the kernel unread.

## Not done

- One device scale for all monitors (the largest); no per-monitor density.
- At a fractional scale the 8x16 font's pixels become one or two device pixels unevenly.
- A layout size that does not map onto the monitor's pixels exactly (266 units at 150 % are 399 of 400 rows) leaves the last device row or column
  undrawn.
- Windows DPI virtualization (above) has these gaps:
  - Below the device scale a unit is part of a device pixel: an odd coordinate a system-aware program sets at 50 % reads back one higher.
  - Messages one program sends another with coordinates are not converted between their awarenesses.
  - Window regions are read back as they were set.
- X programs are not told the density (`Xft.dpi`): they are enlarged.
- The desktop's own text is the enlarged 8x16 font, not a font drawn at the device size.
- A GPU device image (`draw_device_image`) cannot be enlarged by the scaling renderer: callers draw it from the CPU's view.

## Tests

| What | How |
| --- | --- |
| The scaling renderer: a scene at 200 % and 300 % equal to the 1x scene enlarged pixel for pixel (rectangles, outlines, lines, text, images, masks, clips); at 125, 150 and 175 % every device pixel the unit it maps back to (pictures left out); extent, device, read-back, matrices | `tests/run_host_tests.sh GraphicsTest` (`test_scaled_2d`) |
| `MonitorDisplay` at 200 % and 150 %: a 200 % monitor 1:1 in one copy, a 100 % monitor beside it halved, a part of the picture, 300 % at 3/2 with its last columns | `tests/run_host_tests.sh MonitorTest` |
| The desktop at scale 2: the session told 2, a window enlarged, then drawn on the device at twice its place when the program draws at the scale | `tests/run_host_tests.sh GuiTest` (`test_windows_hidpi`) |
| Wayland at 200 % and 150 % (`wl_output.scale` 2, fractional 180, a fractional client's viewported buffer drawn 1:1) | `tests/run_host_tests.sh WaylandServerTest` (`test_mapping`) |
| Windows DPI virtualization at 200 % and 150 %, the change from one to the other (`WM_DPICHANGED` with 144 dpi and the suggested rectangle, applied by DefWindowProc; the unaware window's units unchanged), and DPI hosting (a default-hosting child takes the aware tree's awareness, `SetParent` across awarenesses refused, a mixed-hosting unaware child of an aware window drawn on its own surface and scaled into the window's, put back over what the window paints). An unaware program: its rectangles, client size, surface size and drawing, mouse lParam and `MSG.pt`, `GetCursorPos`, DPI and screen metrics, and moves both from the desktop (`WM_SIZE` in units) and from the program. An aware program: device-pixel rectangles, 192 dpi, its mouse, and its surface drawn 1:1. The desktop in units both ways | `tests/run_host_tests.sh Win32kUserTest` (`test_dpi_virtualization`) |
| No regression at scale 1 in a real run: `tests/windows/user32test.c` in QEMU | `scripts/winetest.sh user32 tcos` |
