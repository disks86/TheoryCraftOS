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
    the layout's. When the device is not a whole number of units (400 rows at 150 % are 266 units, which map back to 399), the layout's far edge
    is stretched to the device's (`ScaledRenderer2D::edge`): the last unit takes the spare pixels, so what reaches the edge covers the screen.
    The window systems that work in device pixels are told the device's size (`IWindowsSession::Monitor::device_width` / `device_height`) and do
    the same: win32k's screen and the X root are the device's size, a monitor or work area that reaches the area's far edge reaches the device's,
    and a maximized window fills it. Going back, the spare pixels count as the last unit, so the desktop still sees that window as the whole area.
  - **The desktop's own drawing** goes through it. Rectangles, lines, shapes and the 8x16 font's pixels are mapped by their edges, so what touches still
    touches. At a whole scale a unit is exactly k x k pixels; at 150 % units alternate between one and two pixels. Images and masks are sampled nearest
    at a whole scale and filtered at a fractional one. What is drawn at the device's resolution is sharp.
  - **GPU images.** A picture that lives on the graphics device (`draw_device_image`, e.g. a Wayland dmabuf or a shared 3D frame made at 1x) is
    enlarged on the device by its edges (`draw_device_image_scaled`: nearest at a whole scale, filtered at a fractional one), not read back
    through the CPU.
  - **Text.** The kernel gives the scaling renderer a text face (`graphics::CellTextFace`, `ScaledRenderer2D::set_text_face`). Each character of the
    desktop's text is then drawn with the bundled "TCOS Mono" (Liberation Mono; "TCOS Fallback" for what it lacks) at the device's resolution,
    anti-aliased and sized to fill its 8 x 16-unit cell on the device, so the layout is the same. Box drawing and block characters (U+2500..U+259F) stay
    the 8x16 font's pixels, which join from cell to cell. Without a face (the renderer's default) text is the 8x16 font enlarged.
- **Monitors at one scale.** `MonitorDisplay` maps the device-scale picture onto each monitor (by the same mapping): a 1:1 copy each.
- **Monitors at different scales** are each drawn at their own (`hardware::MonitorRenderers`, chosen by `DisplayPipeline` in `main.cpp` when the
  enabled monitors' scales differ):
  - Every monitor has a view: a software renderer of its own pixels (presented into its place on the display device) under a
    `ScaledRenderer2D` at the monitor's scale whose origin (`set_origin`) is the monitor's place in the layout.
  - The desktop draws once, in the layout's units, through `graphics::MultiScaleRenderer2D`, which hands every call to each view; each view clips to
    its own pixels. So a 100 % monitor beside a 200 % one gets the desktop drawn at 100 %, and the 200 % one at 200 %, text included (each view has
    the text face), instead of one picture resampled.
  - Results are true only when every view did the thing (a group a view could not begin is closed again on the others). Retained layers and
    `copy_rect` are declined there, so callers draw instead.
  - Sessions are told each monitor's own scale (`IWindowsSession::Monitor::monitor_scale`, from the view that shows the monitor:
    `gui::Window::scale_at`; told again when a monitor's scale or place changes). The Wayland compositor asks each surface to draw at the scale of
    the monitor its window is on (the centre of its toplevel's window geometry; popups and subsurfaces follow their window): the fractional
    `preferred_scale` and `wl_surface.preferred_buffer_scale` are sent when that changes (shown, moved by the desktop to another monitor, the
    monitors changed), so a GTK 4 or Qt 6 window draws at 100 % on a 100 % monitor and at 200 % on a 200 % one.
  - `IRenderer2D::device_views()` / `device_view()` list the views (a `DeviceView`: its units renderer, its device, scale and origin). Program windows
    are drawn on each: 1:1 on the device whose scale is the program's (`draw_window_scaled`), and through that view's units on the others (a picture
    made at 200 % is filtered down for a 100 % monitor).
- **Programs.** Sessions are told the device scale (`IWindowsSession::Monitor::scale_percent`). A window is drawn with `draw_window_scaled` on the device
  renderer (`IRenderer2D::device()`, `scale_percent()`) when its session can: the Wayland compositor draws a buffer made at that scale 1:1 and announces the scale
  (`wl_output.scale`, `preferred_buffer_scale`, fractional `preferred_scale`; see [Wayland.md](Wayland.md)). Windows programs are DPI-virtualized as
  on Windows (below): an aware program's window is drawn 1:1, an unaware one's enlarged. X programs work in device pixels (below) and their windows
  are drawn 1:1 on the device.

## X programs

The kernel X server (`src/X11`) works in device pixels at a scale: the root window, the screen's millimetres (at 96 x the scale dots per inch),
windows, pictures, the pointer and touches. It converts at its side of `IWindowsSession`, as `DesktopBridge` does for Windows: what the desktop gives
(the display and monitors, frame moves, the mouse, touches, drag positions) is multiplied by the scale, and what it reports (window and inside
rectangles, repaints, the pointer, drop positions, strut sizes, the window menu's place) divided. The desktop's frame is the scale's device pixels
around a window (`_NET_FRAME_EXTENTS` says so). Pictures go on the device 1:1 (`draw_window_scaled`), or shrunk on a renderer that cannot draw there.

Clients are told the density through the root's `RESOURCE_MANAGER`: `Xft.dpi` (96 x the scale / 100) and `Xcursor.size` (24 x the scale / 100). A
client's own lines there stay. When the scale changes, each top-level keeps its place in units (its device rectangle times the change; a maximized or
full-screen one fills its area again), clients hear `ConfigureNotify`, and `Xft.dpi` is updated.

## Windows programs on monitors at different scales

As on Windows, win32k's screen is then each monitor's own pixels (`DesktopBridge`'s monitor table): a monitor's top left is its place in units at
the device scale, its size its units at its own scale. So a 200 % monitor A (units 0..800) and a 100 % monitor B beside it (units 800..1600) are
pixels 0..1600 and 1600..2400. The bridge maps rectangles through the monitor at their centre and points through the monitor they are on (one
scale for all monitors: exactly the mapping below). The system DPI is the primary monitor's; `GetDpiForMonitor` gives each monitor's.

- A top-level follows the scale of the monitor its centre is on (`Window::monitor_scale`, `refresh_monitor_scale`, after every change the desktop
  takes): its frame is the desktop's at that scale, and its DPI kind decides its coordinates there - an unaware window is scaled by the monitor, a
  system-aware one by the monitor against its system DPI, a per-monitor aware one works in the monitor's pixels.
- Moved by the desktop (a frame dragged to the other monitor), the window's units are kept, so its pixels are already the new monitor's: a
  per-monitor aware window hears `WM_DPICHANGED` with the new DPI and the rectangle it has. Moved by the program (`SetWindowPos` in its pixels),
  a per-monitor aware one is asked to take the new DPI at the new density (`WM_DPICHANGED` with its size times new / old, which DefWindowProc
  applies) and any other is resized so that its size in its own coordinates stays, as Windows does.
- An unaware or system-aware thread's coordinates (its calls, as `caller_scale`) follow the monitor of its window.
- The desktop draws a window's surface 1:1 on the monitor whose scale is its pixels per unit there (`draw_window_scaled`), resampled on the others.
- When the monitors change, every top-level goes back to where it was in units.

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

- Monitors at different scales:
  - X programs are not told per monitor (one screen, one `Xft.dpi`: the largest scale); their pictures are filtered down on smaller monitors.
  - Wayland has one `wl_output` (its scale the largest), so `wl_surface.enter` names no monitor.
  - Windows: an unaware or system-aware thread's coordinates follow the monitor of its first top-level there, so one program with windows on two
    monitors of different scales sees one of them in the other's coordinates. Messages between programs are not converted.
- With monitors at different scales the desktop is drawn on the CPU (a software renderer per monitor), as the monitor compositor's path was.
- A screen that is not a whole number of units at its scale (400 rows at 150 % are 266 units) is 266 units for Wayland clients too: their logical
  coordinates are the protocol's, so a maximized one's buffer ends a device row short of the edge, where the desktop's background shows.
- Windows DPI virtualization (above) has these gaps:
  - Below the device scale a unit is part of a device pixel: an odd coordinate a system-aware program sets at 50 % reads back one higher.
  - Messages one program sends another with coordinates are not converted between their awarenesses.
  - Window regions are read back as they were set.
- X programs that ignore `Xft.dpi` and the screen size (core-font Xlib programs, the 8x16 core font itself) look small at a scale, as on a Linux
  HiDPI screen. X cursors are drawn as the desktop's pointer size, not at the device's.

## Tests

| What | How |
| --- | --- |
| The scaling renderer: a scene at 200 % and 300 % equal to the 1x scene enlarged pixel for pixel (rectangles, outlines, lines, text, images, masks, clips); at 125, 150 and 175 % every device pixel the unit it maps back to (pictures left out); extent, device, read-back, matrices | `tests/run_host_tests.sh GraphicsTest` (`test_scaled_2d`) |
| A shared 3D frame drawn through the scaling renderer at 200 %: on the device, each pixel 2 x 2, equal to the read-back picture | `tests/run_host_tests.sh CubeRendererTest` (`test_shared`) |
| X at 200 %: the root and windows in device pixels, `Xft.dpi` 192 and `Xcursor.size` 48, window rectangles and frame moves in units both ways, the pointer, the picture 1:1 on the device and shrunk to units, back to 100 % (the place kept, `ConfigureNotify`, 96 dpi), a client's own resources kept | `tests/run_host_tests.sh X11ServerTest` (`test_hidpi`) |
| Windows and X on a 400 x 400 device at 150 % (266 units): the screen and root 400 x 400, a maximized window 400 device pixels across (less X's frame), 266 units for the desktop | `tests/run_host_tests.sh Win32kUserTest` (`test_device_edge`), `X11ServerTest` (`test_hidpi`) |
| Monitors at different scales (`test_multi_scale`): a layout half at 200 %, half at 100 % (origin at unit 32): each view's pixels equal one renderer at its scale for the whole layout; device views, read-back, declined copies and retained layers; matrices with an origin | `tests/run_host_tests.sh GraphicsTest` |
| `MonitorRenderers` (`test_monitor_renderers`): a 200 % and a 100 % monitor on one device, a rectangle across the seam on each at its density and place, the frame read back, a refused update keeping the views | `tests/run_host_tests.sh MonitorTest` |
| Wayland on a 200 % and a 100 % monitor: a window told 240 / buffer scale 2 on the first, 120 / 1 when the desktop moves it to the second, back again, nothing for a move on the same monitor | `tests/run_host_tests.sh WaylandServerTest` |
| The desktop on a 200 % and a 100 % monitor (`test_windows_mixed_scales`): a program window across the seam drawn 1:1 on the 200 % device and through units on the 100 % one | `tests/run_host_tests.sh GuiTest` |
| Windows on a 200 % and a 100 % monitor (`test_per_monitor_dpi`): the screen 2400 x 1200, each monitor's DPI; a per-monitor aware window moved by the desktop to the 100 % monitor hears 96 dpi and keeps its rectangle, moved back by itself hears 192 at twice the size; its surface 1:1 on the 200 % device only; an unaware window keeps its size in its coordinates on both; the cursor both ways | `tests/run_host_tests.sh Win32kUserTest` |
| A 400-pixel device at 150 %: filling the 266-unit layout covers every device pixel; the last unit takes the spare one | `tests/run_host_tests.sh GraphicsTest` (`test_scaled_far_edge`) |
| The desktop's text at 200, 150 and 125 %: drawn with the bundled mono face inside its cells, anti-aliased, box drawing unchanged | `tests/run_host_tests.sh GraphicsTest` (`test_cell_text_face`) |
| `MonitorDisplay` at 200 % and 150 %: a 200 % monitor 1:1 in one copy, a 100 % monitor beside it halved, a part of the picture, 300 % at 3/2 with its last columns | `tests/run_host_tests.sh MonitorTest` |
| The desktop at scale 2: the session told 2, a window enlarged, then drawn on the device at twice its place when the program draws at the scale | `tests/run_host_tests.sh GuiTest` (`test_windows_hidpi`) |
| Wayland at 200 % and 150 % (`wl_output.scale` 2, fractional 180, a fractional client's viewported buffer drawn 1:1) | `tests/run_host_tests.sh WaylandServerTest` (`test_mapping`) |
| Windows DPI virtualization at 200 % and 150 %, the change from one to the other (`WM_DPICHANGED` with 144 dpi and the suggested rectangle, applied by DefWindowProc; the unaware window's units unchanged), and DPI hosting (a default-hosting child takes the aware tree's awareness, `SetParent` across awarenesses refused, a mixed-hosting unaware child of an aware window drawn on its own surface and scaled into the window's, put back over what the window paints). An unaware program: its rectangles, client size, surface size and drawing, mouse lParam and `MSG.pt`, `GetCursorPos`, DPI and screen metrics, and moves both from the desktop (`WM_SIZE` in units) and from the program. An aware program: device-pixel rectangles, 192 dpi, its mouse, and its surface drawn 1:1. The desktop in units both ways | `tests/run_host_tests.sh Win32kUserTest` (`test_dpi_virtualization`) |
| No regression at scale 1 in a real run: `tests/windows/user32test.c` in QEMU | `scripts/winetest.sh user32 tcos` |
