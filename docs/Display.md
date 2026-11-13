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
  (`wl_output.scale`, `preferred_buffer_scale`, fractional `preferred_scale`; see [Wayland.md](Wayland.md)). Other sessions' windows are drawn with
  `draw_window` through the scaling renderer, enlarged as everything was before: Windows programs (win32k's screen and DPI stay 96 per unit) and X programs.

## Not done

- Fractional scales drawn natively (125 / 150 / 175 % keep the stretched 1x picture; the preferred scale is never fractional).
- One device scale for all monitors; no per-monitor density.
- Windows programs are not told the DPI (per-monitor DPI awareness, `WM_DPICHANGED`, DPI virtualization) and X programs not the density (`Xft.dpi`):
  both are enlarged.
- The desktop's own text is the enlarged 8x16 font, not a font drawn at the device size.
- A GPU device image (`draw_device_image`) cannot be enlarged by the scaling renderer: callers draw it from the CPU's view.

## Tests

| What | How |
| --- | --- |
| The scaling renderer: a scene at 2x and 3x equal to the 1x scene enlarged pixel for pixel (rectangles, outlines, lines, text, images, masks, clips), extent, device, read-back, matrices | `tests/run_host_tests.sh GraphicsTest` (`test_scaled_2d`) |
| `MonitorDisplay` at render scale 2: a 200 % monitor 1:1 in one copy, a 100 % monitor beside it halved, a part of the picture, 300 % at 3/2 with its last columns | `tests/run_host_tests.sh MonitorTest` |
| The desktop at scale 2: the session told 2, a window enlarged, then drawn on the device at twice its place when the program draws at the scale | `tests/run_host_tests.sh GuiTest` (`test_windows_hidpi`) |
| Wayland at scale 2 | `tests/run_host_tests.sh WaylandServerTest` (`test_mapping`) |
