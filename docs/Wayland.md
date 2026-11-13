# The Wayland compositor

TheoryCraftOS has a Wayland compositor in the kernel (`src/Wayland`). Linux programs reach it the usual way, through `XDG_RUNTIME_DIR` and
`WAYLAND_DISPLAY`. The desktop is its window manager: each xdg toplevel is a window of the desktop, made by the same code that shows the Windows
programs' windows and the X server's windows. Native, Win32, X11 and Wayland windows therefore get the same frame, title bar, buttons, taskbar
button and Alt+Tab place, and the user moves, sizes, minimizes, maximizes, activates and closes them the same way.

| Part | Where |
| --- | --- |
| Connections, the wire, objects, the registry and globals, protocol errors | `src/Wayland/WaylandServer.{hpp,cpp}`, `WaylandInternal.hpp` |
| Surfaces, regions, subsurfaces, shm pools and buffers, xdg-shell, xdg-decoration, drawing | `src/Wayland/WaylandSurfaces.cpp` |
| The desktop's view: window list, info, drawing, moves, activation, commands | `src/Wayland/WaylandDesktop.cpp` |
| The seat: pointer, keyboard, cursor | `src/Wayland/WaylandInput.cpp` |
| HID usage to evdev codes, the xkb keymap | `src/Wayland/WaylandKeymap.cpp` |
| AF_UNIX listener with SCM_RIGHTS, the compositor's kernel thread | `src/Wayland/WaylandSocketServer.{hpp,cpp}` |
| `zwp_linux_dmabuf_v1`: GPU buffers | `src/Wayland/WaylandDmabuf.cpp` |
| The selection, the desktop's clipboard, drag and drop | `src/Wayland/WaylandData.cpp` |
| Buffer scale and transform, `wp_viewporter`, `wp_fractional_scale_v1` | `src/Wayland/WaylandScale.cpp` |
| Activation, cursor shapes, primary selection, relative pointer and constraints, presentation time, idle inhibit, xdg-foreign, pings | `src/Wayland/WaylandExtras.cpp` |
| `pipe` / `pipe2` for Linux programs (what clipboard transfers go through) | `src/Process/LinuxSockets.cpp` |
| GPU images over the kernel's Direct3D layer (kept apart: the rest builds without it) | `src/Wayland/Dxg/WaylandDxgImage.{hpp,cpp}` |
| dmabufs: `/dev/dxg`'s export and import, the descriptor, the kernel's export object | `src/Process/DxgBufferFile.hpp`, `LinuxFiles.cpp`, `Win32kDxg.cpp`, `Kernel/Dxgkrnl/DxgKernel.{hpp,cpp}` |
| Files in memory (`memfd_create`), mappable and readable by the kernel | `src/Process/MemoryFile.{hpp,cpp}`, `LinuxFiles.cpp`, `LinuxSyscalls.cpp` |
| The desktop side (the same as for Win32 and X windows) | `src/Desktop/Win32Windows.{hpp,cpp}` over `desktop::IWindowsSession` |
| Start-up | `src/main.cpp` (after the X server) |

## How it fits together

`wayland::Server` implements `desktop::IWindowsSession`, as win32k's bridge and `x11::Server` do. The desktop holds a third `Win32Windows`
(`Desktop::set_wayland_session`) over it, which makes the frames, forwards the user's actions and hands keys and the mouse to the compositor
through the same input taps. `IWindowsSession::mouse_left` (new; a no-op for the others) tells a session that the pointer went to a window that is
not one of its own, so the compositor can send `wl_pointer.leave`.

- **Globals.** `wl_compositor` 6, `wl_subcompositor` 1, `wl_shm` 2, `wl_seat` 8, `wl_output` 4 (the primary monitor), `wl_data_device_manager`
  3 (copy and paste, drag and drop), `xdg_wm_base` 6, `zxdg_decoration_manager_v1` 1, `wp_viewporter` 1, `wp_fractional_scale_manager_v1` 1, `xdg_activation_v1` 1, `wp_cursor_shape_manager_v1` 1,
  `zwp_primary_selection_device_manager_v1` 1, `zwp_relative_pointer_manager_v1` 1, `zwp_pointer_constraints_v1` 1, `wp_presentation` 1,
  `zwp_idle_inhibit_manager_v1` 1, `zxdg_exporter_v2` 1, `zxdg_importer_v2` 1, `zwp_linux_dmabuf_v1` 4 and `wp_linux_drm_syncobj_manager_v1` 1 (the last two while
  the transport can make GPU images and timelines: the kernel sets that up when it has the Direct3D layer).
- **One coordinate space.** The compositor's screen is the desktop's window manager area; its monitors and work areas come from `set_display`.
  The desktop's "surface" rectangle of a window is the client's window geometry (`xdg_surface.set_window_geometry`, else its buffer), so a
  client-side shadow is outside the frame and is neither drawn nor hit.
- **Decorations.** A client that asks for an `zxdg_toplevel_decoration_v1` always gets `server_side`: the desktop draws its frame, like a Win32
  window's. A client that does not ask (GTK) draws its own and moves or sizes itself with `xdg_toplevel.move` / `resize`, which become the
  desktop's drag of the frame (`take_drag`), as a Win32 program's custom title bar does; its right click on that title bar sends
  `xdg_toplevel.show_window_menu`, and the desktop opens the frame's window menu there (`take_window_menu`: Restore, Minimize, Maximize, Close, the
  menu every frame has on its title bar and on Alt+Space).
- **Configure.** The first commit gets a configure with size 0 x 0 (the client chooses), `configure_bounds` (the work area inside a frame) and
  `wm_capabilities` (maximize, fullscreen, minimize). The user sizing the frame sends a configure with the new size; until the client commits after
  acknowledging it, the desktop is told the asked-for size and paints what the buffer does not cover black, as for a Win32 window that has not caught
  up. Maximize, fullscreen (the monitor, no frame) and activation are configure states. Minimizing is the desktop's (no state is sent); the close
  button sends `xdg_toplevel.close`. Minimum and maximum sizes limit the frame; equal ones make the window not sizable.
- **Popups** (menus, tooltips) are frameless desktop windows that do not activate, placed by their positioner relative to the parent's window
  geometry: anchor, gravity and offset, then kept on the parent's monitor with the flip, slide and resize adjustments the client allows. A press
  outside the popups closes them all (`popup_done`, innermost first), as does activating another window. A grabbing popup takes the keyboard.
- **Pictures.** A `wl_shm` pool is a file in memory the client passed with `SCM_RIGHTS`. The desktop draws a window's committed buffer straight from
  that memory (`draw_window`), so a frame costs one copy, the same as for Win32 and X windows. Subsurfaces are drawn over their parent in their
  stacking order; their synchronized commits are applied with the parent's. Formats: ARGB8888, XRGB8888, ABGR8888 and XBGR8888. A buffer is
  released when another one replaces it.
- **Frame callbacks** are answered when the desktop has drawn the surface, so a client runs at the desktop's redraw rate and stops drawing while
  its window is hidden or minimized. A cursor surface's are answered at its commit.
- **Pointer.** Focus is the surface under the pointer, subsurfaces and input regions included. While a button is held it stays with the surface the
  press went to (the implicit grab), and `capture_window` keeps the desktop sending the pointer there. Buttons are `BTN_LEFT`, `BTN_RIGHT` and
  `BTN_MIDDLE`; a wheel notch is `axis_source` wheel, `axis_value120` 120 (`axis_discrete` 1 before version 8) and `axis` 15. `set_cursor` from the
  client the pointer is over becomes the desktop's pointer picture (un-premultiplied); a null surface hides it.
- **Keyboard.** Focus is the active toplevel, or its topmost grabbing popup. Key codes are evdev codes (HID usage mapped as Linux's hid-input does).
  The keymap is a complete US xkb_v1 text, with no includes, in a sealed file in memory that every client maps read-only. Modifier masks are the
  real modifiers: Shift 1, Lock 2, Control 4, Mod1/Alt 8, Mod2/NumLock 16, Mod4/Super 64. Keys repeat on the client side (`repeat_info` 25/s after
  600 ms), so the desktop's repeated presses of a held key are dropped.

## The other protocols, and pings

- **New windows are active.** A toplevel that is mapped takes the activation and the keyboard, as a program's window does when it is first shown.
  The desktop takes the activation from what the session reports, so before this a Wayland window got no keys until it was clicked. (Found on the
  real kernel: the libwayland test client waited for its activated configure.)
- **Activation** (`xdg_activation_v1`): tokens are made by the compositor and spent once; `activate` activates the surface's window as a click would.
- **Cursor shapes** (`wp_cursor_shape_v1`): the 34 named shapes become the desktop's own cursors (the `OCR_*` shapes it has for Windows programs).
- **Primary selection** (`zwp_primary_selection_v1`): the middle-button selection between Wayland clients, offered with the keyboard as the
  clipboard's selection is; pastes go to the source with the client's descriptor. The desktop has no primary selection to bridge it to.
- **Relative pointer and constraints** (`zwp_relative_pointer_v1`, `zwp_pointer_constraints_v1`): every motion is also sent as it came. A lock keeps the
  pointer where it is (its hint is where it is put back), a confinement keeps it in the surface (or its region), while the surface has the pointer and
  its window is active; the desktop then holds the pointer for the program as for a Windows program's `ClipCursor`, and Ctrl+Alt takes it back.
  One-shot constraints end at their first release; persistent ones come back.
- **Presentation time** (`wp_presentation`, clock `CLOCK_MONOTONIC`): a commit's feedback is `presented` when the desktop draws the picture (time,
  60 Hz refresh, a sequence count) or `discarded` when a newer picture replaced it first.
- **xdg-foreign** (`zxdg_exporter_v2`, `zxdg_importer_v2`): one client's toplevel exported as a handle becomes the parent of another client's
  toplevel (the desktop shows it as owned); when the exported window goes, the imports hear `destroyed` and the dialogs lose their parent.
- **Pings and the minimized state.** `xdg_wm_base.ping` goes when the user activates or closes a window; a client that has not answered after five
  seconds is shown as not responding (its windows' titles end in "(Not Responding)", as on Windows) until it does. `xdg_wm_base` is version 6, so
  a minimized toplevel is configured `suspended`.

## Scale, transforms, viewports

The desktop has no scale of its own, so the output's scale is 1 (`wl_output.scale`, `wl_surface.preferred_buffer_scale` and `preferred_buffer_transform`
for version 6 surfaces, `wp_fractional_scale_v1.preferred_scale` 120). What a client asks of its buffer is honoured: `set_buffer_transform` (the eight
rotations and flips, as Weston maps them), `set_buffer_scale` (1 .. 16; a version 6 surface whose buffer is not a multiple is `invalid_size`) and
`wp_viewport` (a crop in 24.8 and a destination size, with the protocol's `bad_value`, `bad_size`, `out_of_buffer`, `no_surface`). They decide the
surface's size (input, the window's size, configure), and the desktop draws the buffer through one affine map (`IRenderer2D::draw_image_transformed`, done
by a GPU renderer's texture units; a stretch filters bilinearly). A GPU buffer with such a mapping is drawn from the CPU's view; a renderer that cannot
transform stretches an upright crop and leaves a turned picture undrawn. A surface with a mapping is repainted whole at each commit.

## Copy and paste, drag and drop

- **Touch** (`wl_touch`; the seat says pointer, keyboard and touch). The desktop reads the touchscreens (`hardware::ITouch`: QEMU's
  virtio-multitouch, `src/Hardware/Touch/Virtio`, and I2C-HID touchscreens) and offers each finger that lands on a Wayland window to the compositor
  (`IWindowsSession::touch`). A client with a `wl_touch` gets it: `down` (with the surface under the finger and its place in it), then `motion`
  (in that surface's coordinates, wherever the finger goes) and `up`, each followed by `frame`; a cancelled contact is `cancel` (all of the client's).
  A finger over a client without a `wl_touch` is refused, and the desktop moves the pointer with it instead (the first such finger is the left
  button), as it does over its own windows and Win32 and X windows.
- **Pens** (`zwp_tablet_manager_v2` version 1, `src/Wayland/WaylandTablet.cpp`). A client's tablet seat is told of one tablet ("TheoryCraftOS pen")
  and two tools, the pen and its eraser, each with pressure and tilt. The desktop hands the compositor the pens (`hardware::IPen`, through
  `IWindowsSession::pen`); over a surface of a client with a tablet seat the tool of the end near the screen comes into proximity of that surface
  (`proximity_in`), then each report is `motion`, `pressure` (0 to 65535), `tilt`, `down` / `up` as the tip touches or lifts, `button` for the barrel
  buttons (`BTN_STYLUS`, `BTN_STYLUS2`) and a `frame`. While the tip touches, the surface keeps the pen wherever it goes; otherwise it follows the
  surface under it (`proximity_out` from the old one). Over a client without a tablet seat, or nowhere, the pen is refused and the desktop moves the
  pointer with it (`hardware::PenMouse`'s rules, done by the input pump since the desktop reads the pens itself). No pads, rings, strips or dials, no
  hardware serials or ids, no distance, rotation, slider or wheel, and a tool's `set_cursor` is ignored (the desktop draws its own pointer).
- **The selection** is a client's `wl_data_source` or the desktop's clipboard (its text, or its picture). The client with the keyboard is offered it (`wl_data_offer` with
  its types, then `wl_data_device.selection`) when the keyboard comes to it and whenever the selection changes. A paste (`wl_data_offer.receive` with
  a pipe) is passed on to the source's client as it is (`wl_data_source.send` with the same descriptor), or, for the desktop's text or picture, the
  transport writes it into the pipe and closes it.
- **The desktop's clipboard follows** (`kernel::Clipboard`, `Server::set_clipboard`). A client's selection with text (`text/plain;charset=utf-8`,
  `text/plain`, `UTF8_STRING`, `STRING`, `TEXT`) is read at once through a pipe the transport makes (`TransportRequest`, up to 16 MiB) and put on
  the clipboard, so Windows and desktop programs paste it; text the desktop's programs copy becomes the selection (the client's source is
  cancelled). The clipboard is touched only outside the compositor's lock (its observer keeps the Windows clipboard the same and takes the system's
  locks): `tick()`, which the transport calls each round, looks at it.
- **Pictures** go as PNG files (`image/png`) when the server has the codecs (`Server::set_clipboard_images`: `src/Wayland/Media/WaylandClipboardPng.cpp`
  over `src/Media`'s PNG codec, given by `main.cpp`, so the compositor itself builds without the media code). A picture the desktop copies is encoded
  once (RGBA when some pixel is not opaque, else RGB) and offered as the one type `image/png`; every paste gets the same bytes. A client's selection
  with `image/png` and no text type is read like text and decoded onto the clipboard as its picture (an animation's first frame); a file the decoder
  refuses changes nothing. Text wins over a picture when a source offers both.
- **Descriptors pass through.** A descriptor a client sends that the compositor does not read itself (a pipe's end) is kept with its use of its token
  and sent on unchanged; one the compositor lets go of is released by the transport's thread (`SocketServer::Graveyard`), never under the
  compositor's lock.
- **Drag and drop** between Wayland clients: `start_drag` while a button is held takes the pointer; the surface under it is entered with an offer
  of the source's types, followed and left; version 3's actions are negotiated (the destination's preference, else copy, move, ask) and told to
  both; a release over a destination that accepted a type drops (`drop`, `dnd_drop_performed`), the destination receives as for a paste and
  `finish`es (`dnd_finished`); anything else cancels. A drag without a source stays within its client.
- **Linux pipes.** Linux programs here had no `pipe()`; `pipe` and `pipe2` are now AF_UNIX stream pairs shut one way each (`O_CLOEXEC`,
  `O_NONBLOCK`), so they pass with `SCM_RIGHTS` like any socket. `fstat` calls them sockets, not FIFOs.

## GPU buffers (zwp_linux_dmabuf_v1)

A client that renders on the GPU hands the compositor the image it rendered into, not a copy of its pixels. On this system a GPU image becomes a descriptor
through `/dev/dxg` (`TCOS_DXG_IOCTL_EXPORT`, [win32k/Dxg.md](win32k/Dxg.md)), which is what the Linux Vulkan driver's `vkGetMemoryFdKHR` does
(`VK_EXT_external_memory_dma_buf`, [Vulkan.md](Vulkan.md)); the descriptor is the counterpart of a Linux dmabuf, and the client passes it in
`zwp_linux_buffer_params_v1.add` as on Linux.

- **What is taken.** One plane (every format offered has one), offset 0 and the image's own row pitch, the linear layout (`DRM_FORMAT_MOD_LINEAR`, or
  `DRM_FORMAT_MOD_INVALID`, taken as linear), no flags, a size no larger than the image. Formats ARGB8888, XRGB8888, ABGR8888 and XBGR8888: the format says
  whether alpha counts; the order of the channels is the image's own (the Direct3D layer knows it), so a driver's swizzle (virgl keeps RGBA images as BGRA)
  never shows. `create` answers `created` with a `wl_buffer` of the server's ids (from 0xFF000000; no `delete_id` when it goes), or `failed`; `create_immed`
  answers a buffer that cannot be made with `invalid_wl_buffer`. Every other error of the protocol is checked (`already_used`, `plane_idx`, `plane_set`,
  `incomplete`, `invalid_format`, `invalid_dimensions`, `out_of_bounds`).
- **Feedback (version 4).** `get_default_feedback` and `get_surface_feedback` answer one tranche with the four formats, linear, on the main device
  `/dev/dxg` (`TCOS_DXG_DEVICE_NUMBER`, the `st_rdev` an `fstat` of `/dev/dxg` gives), and a format table in a sealed file in memory (16-byte entries:
  format, padding, modifier). Version 4 sends no format or modifier events, as the protocol says. Without a maker of descriptors (the host harness) the
  global is offered as version 3.
- **Drawing, with no copy.** When the desktop draws the window (`draw_window`), the device's own image is lent (`dxgk::Kernel::acquire_export_image`: queued
  work on it done first, made resident and pinned while it is drawn) and drawn in place (`IRenderer2D::draw_device_image`) when the desktop's renderer draws on
  that device: the frame never passes through the CPU. On another device or the CPU renderer the picture is drawn from the CPU's view of it (`read_export`: one
  copy, as a `wl_shm` buffer costs); a renderer that refused the device's image once is not asked again. A picture the GPU is still drawing is not shown that
  time: the window is marked for a repaint and the desktop comes back for it.
- **Lifetime.** The descriptor holds a `dxgk::ExportedAllocation`, which holds the allocation: a buffer keeps showing after the client destroyed its image or
  exited, until the buffer is replaced or destroyed. Dropping the last use takes the Direct3D layer's lock, never held by anyone who drops one.
- **CPU writes.** A client may `mmap` its dmabuf and write it with the CPU (GBM-style clients): the mapping is a copy of the image
  ([win32k/Dxg.md](win32k/Dxg.md)); at each commit of a buffer made from it the compositor sends the copy to the image, when a writable mapping is still
  there and a checksum says the copy changed since it was last moved, so a client that renders on the GPU is never overwritten by an old copy.
- **Explicit sync** (`wp_linux_drm_syncobj_manager_v1`). A timeline is a fence of the Direct3D layer exported with `/dev/dxg`
  (`TCOS_DXG_IOCTL_EXPORT_SYNC`, [win32k/Dxg.md](win32k/Dxg.md)): the counterpart of a DRM sync object. A commit with a buffer names an acquire point and a
  release point (both, a dmabuf, the release after the acquire on one timeline: every rule of the protocol is checked). A commit whose acquire point is not
  reached waits while the surface keeps showing what it showed; it is applied when the point is found reached, at the next request of any client or the next
  time the desktop draws (which it is asked to do). One commit waits per surface: a newer one replaces it and the older buffer is let go (release point,
  `wl_buffer.release`). A release point is signalled when another buffer replaces the buffer, the same buffer is committed again, or the surface goes.
  A synchronized subsurface's commit does not wait for its acquire point.
- **Release.** As for `wl_shm`: when another buffer replaces it. The desktop reads the image while it draws (under the compositor's lock), so a client that
  re-renders a released image never races the desktop; a client that re-renders the attached image (as `tests/linux/waylandvktest.c` does) may be drawn
  half-way through a frame, as on any compositor that samples a buffer it did not release.
- **Locking.** UI lock, then the compositor, then the Direct3D layer (which never calls out holding its lock). `Wayland/Dxg` is the only part that knows the
  Direct3D layer; the transport is given its maker (`SocketServer::set_gpu_images`, main.cpp), which also turns the global on.

## Files in memory

Wayland shares pictures through file descriptors, so the Linux layer now has `memfd_create` (with `MFD_CLOEXEC` and `MFD_ALLOW_SEALING`),
`fallocate` (mode 0, files in memory only) and `F_ADD_SEALS` / `F_GET_SEALS`. A file in memory (`process::MemoryFile`) is an ordinary `IFile`
in an `OpenObject`, so `read`, `write`, `lseek`, `ftruncate`, `fstat`, `dup` and `SCM_RIGHTS` work on it unchanged. `mmap` of one maps its
pages shared (`MAP_SHARED`) or as a copy-on-write view (`MAP_PRIVATE`); a writable shared mapping needs a writable descriptor, so the sealed keymap
can only be mapped privately or read-only. Other files still cannot be mapped.

The bytes are one `MemoryBlock`: page-aligned memory with a reference count, held by the file, by each mapping and by each kernel reader (the
compositor). Writes and shrinking keep the block; growing past it moves the contents to a bigger one. Mappings made before keep the old block,
which stays valid as long as anyone holds it, so memory stays safe but the sharing ends; a client maps the file again after growing it, as Wayland
clients do around `wl_shm_pool.resize`.

## Reaching it

`wayland::SocketServer` listens on the system's AF_UNIX sockets at `/run/user/0/wayland-0` of the Linux programs' tree, which is
`/Linux/run/user/0/wayland-0` on the system's file system (`filesystem::layout::kRuntimeFolder`, `kWaylandDisplay`). Linux programs get
`XDG_RUNTIME_DIR=/run/user/0` and `WAYLAND_DISPLAY=wayland-0` in their default environment, next to `DISPLAY=:0`, so toolkits that support both
pick Wayland.

A descriptor a client passes that is a file in memory is read in place; any other kind reaches the compositor as one with no memory (a pool made from
it is `invalid_fd`). Descriptors the compositor sends go with the first bytes of the next send, which is never later than the event that uses them.

Locking: one `kernel::Mutex` guards the compositor. The desktop calls in under its UI lock, so the order is UI lock, then compositor. The transport
moves bytes and descriptors under the system lock and feeds the compositor with that lock released. Input and drawing wake the transport's thread
through a flag set under the system lock. A `MemoryFile` has its own small mutex for which block is current.

## Tests

| What | How |
| --- | --- |
| Files in memory (including readers on four threads while the file grows and shrinks); registry and globals; toplevels with the desktop (configure, ack, map, move, size, maximize, minimize, fullscreen, close, decorations, unmap); window geometry; popups and positioners; subsurfaces; drawing from the client's memory; frame callbacks; the seat; 21 kinds of protocol error; truncated, chunked, bit-flipped and random-word streams (about 4000 sessions); the AF_UNIX transport with `SCM_RIGHTS`; 8 client threads plus a desktop thread | `tests/run_host_tests.sh WaylandServerTest` |
| Real libwayland-client + libxkbcommon client (`tests/wayland/wl_client.c`, 31 checks, also four at once), `foot`; `wl-copy` / `wl-paste` (the desktop's clipboard pasted, a copy pasted by another client and landing on the desktop's clipboard; a picture both ways as `image/png`) | `tests/wayland_interop.sh` |
| Activation (a token spent once), cursor shapes, relative motion, a persistent lock (and its coming back), a one-shot confinement, primary selection between clients, presentation feedback presented and discarded, idle inhibitors, xdg-foreign parents (and their going), pings and "(Not Responding)", the suspended state | `tests/run_host_tests.sh WaylandServerTest` |
| Buffer transforms (all eight), scale, viewport crop and stretch, a turned GPU buffer, fractional scale, every rule of `wp_viewport` and the scale | `tests/run_host_tests.sh WaylandServerTest` |
| Copy and paste with the desktop's clipboard and between clients (offers, types, descriptors passed on, the clipboard both ways, cancelling; pictures as `image/png` both ways with truncated and bit-flipped files), drag and drop (enter, motion, accept, actions, drop, finish, cancelling, errors) | `tests/run_host_tests.sh WaylandServerTest` |
| A Linux program (x86-64, AArch64, RISC-V) through the OS's own system calls on the CPU emulator: memfd, mmap, fallocate, seals, SCM_RIGHTS both ways; `pipe2`, the desktop's text pasted, its own selection read into the desktop's clipboard and pasted back into itself through its own pipe (46 checks) | `tests/wayland_emulated.sh` |
| `zwp_linux_dmabuf_v1` with a stand-in GPU image (formats and modifiers, `create` / `created` / `failed`, `create_immed`, server ids, release, a picture still being drawn, every protocol error, about 1600 fuzzed sessions); GPU buffers over the real Direct3D layer: export and import rules, the kernel's reads, an export outliving its allocation and its process, a dmabuf over the AF_UNIX transport drawn in place on its device (pixel for pixel), on the CPU and on another device, four client threads exporting and repainting while the desktop draws (also under ThreadSanitizer) | `tests/run_host_tests.sh WaylandServerTest` |
| A Linux program renders with Vulkan (`libvulkan.a`), exports the image (`vkGetMemoryFdKHR`), shows it through the compositor frame after frame, imports it again, checks `/dev/dxg`'s refusals (88 checks); the desktop draws it in place on the device and from the CPU (x86-64, AArch64, RISC-V on the CPU emulator) | `tests/wayland_dmabuf_emulated.sh` |

`tests/linux/waylandvktest.c` has also been run in QEMU on the real kernel (`TCOS_AUTORUN=/Linux/usr/bin/waylandvktest`, argument `stay`), on the
software renderer (standard VGA) and on virgl (`-device virtio-gpu-gl-pci -display egl-headless`; the screen grabbed over `-vnc unix:...`, since QMP's
`screendump` has no surface in that mode): 117 checks passed on both, the frame showed in a framed desktop window, and on virgl the kernel's count after
the run (`autorun: wayland GPU buffer draws in place 33, from the CPU 0`, `wayland::dxg_image_draws`) shows every frame drawn in place by the desktop's GPU
renderer.

It has also been run in QEMU: the kernel started `tests/linux/waylandtest.c` (`TCOS_AUTORUN=/Linux/usr/bin/waylandtest`, argument `stay`), which
passed its 30 checks with real page tables behind its memfd mappings; its window showed as an ordinary desktop frame with a taskbar button and the
picture it drew last.

`tests/wayland_interop.sh` runs the compositor code on the development machine (`tests/wayland/WaylandHostServer.cpp`), with host descriptors.
`foot` and GTK 4 (`zenity --info`, with `GSK_RENDERER=cairo`) showed their windows correctly there.

## Plan: what GPU clients still need

Real toolkits reach the GPU through Mesa (EGL, GBM) or the Vulkan loader, and both speak to the compositor through libwayland-client. In the order the work
depends on itself:

1. **A dynamic loader for Linux programs: done** (`docs/LinuxDynamicPrograms.md`). The kernel maps a program's interpreter (glibc's `ld.so`), which
   loads the shared libraries itself. A libwayland-client + libxkbcommon client (`tests/wayland/wl_client.c`, built with the development machine's
   glibc) passes its 24 checks against this compositor inside the OS, on the real kernel in QEMU and on the emulator.
2. **`VK_KHR_wayland_surface` in the shared Vulkan driver: done** ([Vulkan.md](Vulkan.md)). `libvulkan.so.1` (installed where the Khronos loader would be)
   makes a swapchain's images shareable allocations, exports each once and makes it a `wl_buffer` with `create_immed`; a present is attach, damage and
   commit (FIFO waits for the frame callback), an acquire takes an image whose buffer was released. The protocol goes through the program's own
   libwayland-client (found with `dlsym`), on an event queue of the swapchain's. `tests/linux_vulkan_wayland_emulated.sh` runs a glibc + libwayland +
   xdg-shell program through it.
3. **EGL and GBM: done.** `libEGL.so.1` of `src/Linux/libegl` ([OpenGL.md](OpenGL.md#linux-egl-and-opengl-es)) is EGL 1.5 with OpenGL ES 2.0/3.0 and
   OpenGL 3.3 over the kernel's OpenGL (`src/OpenGl`), reached through `/dev/dxg`; a window surface is a `wl_egl_window` whose frames are GPU buffers (gbm buffer
   objects of `/dev/dxg` made `wl_buffer`s with `create_immed`; the kernel copies each frame into the image, and the desktop draws it in place) or, without
   `zwp_linux_dmabuf_v1`, `wl_shm` buffers. `libgbm.so.1` of `src/Linux/libgbm` makes buffer objects over the Direct3D layer, and EGL also draws into its
   `gbm_surface`s and makes images of dmabufs (`EGL_EXT_image_dma_buf_import`). `tests/linux_egl_wayland_emulated.sh` runs a glibc + libwayland-egl + xdg-shell
   program and a GBM program through them. Still to do: the kernel's GL drawing into the GPU buffer itself (now its frame is copied there by the CPU).
4. **YUV and several planes** (video players' direct output): the Direct3D layer needs NV12 / P010 formats (with planes at offsets of one allocation), the
   protocol handling more than plane 0, and the desktop's renderers a YUV-to-RGB draw (a shader on the device renderer, the existing conversion code of
   `src/Hardware/VideoCodec` on the CPU).

## Not done

- **GPU buffers.** No `wl_drm`. Feedback never changes (one GPU; no scanout tranche, since the desktop always composites). Only single-plane 8-bit RGB formats, linear, offset 0 and the image's own
  pitch (no YUV video planes, no sub-rectangles of an image, no y-inverted pictures). Implicit sync is the GPU's queue: the desktop draws what the image holds
  once the work queued on it is done. A release point is signalled when the compositor lets the buffer go, which for the device renderer is when the draw
  was submitted, not when the GPU finished it. A dmabuf's `mmap` is a CPU copy, not the image itself: coherent at `DMA_BUF_IOCTL_SYNC` and (for
  writes) at each commit, so a client that writes with the CPU and renders on the GPU into the same image between two syncs sees one or the other. EGL's frames are GPU buffers, but the kernel's GL copies each frame into the buffer's image with the CPU instead of drawing there (see the plan above).
  Not tried on a real GPU (only the software renderer and virgl).
- **Protocols.** No text input (`zwp_text_input_v3`: the system has no input method yet), layer shell (`zwlr_layer_shell_v1`: the desktop owns its
  panels and background, and its window list has no layers below normal windows). Touches have no shape or orientation events and no touch-to-pointer fallback inside a client that has a `wl_touch` (a client that binds
  one gets every finger that lands on its surfaces). Idle inhibitors are only counted: the desktop has no idle blanking to hold off. Presentation times are the
  desktop's draw, not the display's scan-out, and the refresh is taken to be 60 Hz.
- **Clipboard and drag and drop.** Text and PNG pictures reach the desktop's clipboard; other types (HTML, URIs, other image formats such as
  `image/jpeg`) stay between Wayland clients, and the desktop's pictures are offered only as `image/png`. A drag's icon surface is not drawn; drags to and from the desktop's own windows (the File Explorer's files)
  and Windows or X programs are not bridged. Selection and drag serials are not checked.
- **Scale.** The output's scale is 1 because the desktop has none: a HiDPI desktop would need the compositor's logical coordinates apart from its
  pixels (input, configure, geometry). There is one `wl_output` (the primary monitor), and surfaces only get `enter` for it. A cursor surface's
  mapping is ignored.
- **Window manager.** The window menu has no Move or Size items (the desktop moves windows by their title bar only). A popup's `reactive` flag and parent size are
  ignored. Activation tokens are not tied to the serial of an input event (any client can get one).
- **Shared memory semantics.** A file in memory that grows past its block moves (see above). A pool may be mapped past its file's size but within
  its block, where Linux would fault.
- **Windows programs.** Wayland needs descriptor passing, which Windows' `AF_UNIX` lacks, so only Linux programs are clients.
- **Real programs in the OS.** Only the test client has been run inside the OS with the real libwayland (`tests/linux_dynamic_emulated.sh`, and
  QEMU). foot and GTK applications run against the same compositor code on the development machine (`tests/wayland_interop.sh`), not yet inside the
  OS: they need more libraries in the disk image, and probably more system calls.
