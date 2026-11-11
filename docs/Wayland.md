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
  3 (objects only, nothing is transferred), `xdg_wm_base` 5, `zxdg_decoration_manager_v1` 1 and `zwp_linux_dmabuf_v1` 3 (while the transport can make GPU
  images: the kernel sets that up when it has the Direct3D layer).
- **One coordinate space.** The compositor's screen is the desktop's window manager area; its monitors and work areas come from `set_display`.
  The desktop's "surface" rectangle of a window is the client's window geometry (`xdg_surface.set_window_geometry`, else its buffer), so a
  client-side shadow is outside the frame and is neither drawn nor hit.
- **Decorations.** A client that asks for an `zxdg_toplevel_decoration_v1` always gets `server_side`: the desktop draws its frame, like a Win32
  window's. A client that does not ask (GTK) draws its own and moves or sizes itself with `xdg_toplevel.move` / `resize`, which become the
  desktop's drag of the frame (`take_drag`), as a Win32 program's custom title bar does.
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
| Real libwayland-client + libxkbcommon client (`tests/wayland/wl_client.c`, 31 checks, also four at once), `foot` | `tests/wayland_interop.sh` |
| A Linux program (x86-64, AArch64, RISC-V) through the OS's own system calls on the CPU emulator: memfd, mmap, fallocate, seals, SCM_RIGHTS both ways | `tests/wayland_emulated.sh` |
| `zwp_linux_dmabuf_v1` with a stand-in GPU image (formats and modifiers, `create` / `created` / `failed`, `create_immed`, server ids, release, a picture still being drawn, every protocol error, about 1600 fuzzed sessions); GPU buffers over the real Direct3D layer: export and import rules, the kernel's reads, an export outliving its allocation and its process, a dmabuf over the AF_UNIX transport drawn in place on its device (pixel for pixel), on the CPU and on another device, four client threads exporting and repainting while the desktop draws (also under ThreadSanitizer) | `tests/run_host_tests.sh WaylandServerTest` |
| A Linux program renders with Vulkan (`libvulkan.a`), exports the image (`vkGetMemoryFdKHR`), shows it through the compositor frame after frame, imports it again, checks `/dev/dxg`'s refusals (88 checks); the desktop draws it in place on the device and from the CPU (x86-64, AArch64, RISC-V on the CPU emulator) | `tests/wayland_dmabuf_emulated.sh` |

It has also been run in QEMU: the kernel started `tests/linux/waylandtest.c` (`TCOS_AUTORUN=/Linux/usr/bin/waylandtest`, argument `stay`), which
passed its 30 checks with real page tables behind its memfd mappings; its window showed as an ordinary desktop frame with a taskbar button and the
picture it drew last.

`tests/wayland_interop.sh` runs the compositor code on the development machine (`tests/wayland/WaylandHostServer.cpp`), with host descriptors.
`foot` and GTK 4 (`zenity --info`, with `GSK_RENDERER=cairo`) showed their windows correctly there.

## Not done

- **GPU buffers.** `zwp_linux_dmabuf_v1` is version 3: no feedback objects (version 4: `get_default_feedback`, the format table, the main device),
  which newer Mesa and wlroots-based clients prefer but do not need. No `wl_drm`. Only single-plane 8-bit RGB formats, linear, offset 0 and the image's own
  pitch (no YUV video planes, no sub-rectangles of an image, no y-inverted pictures). No explicit synchronization (`linux-drm-syncobj`, implicit fences): the
  desktop draws what the image holds when it draws. A dmabuf's `mmap` is a CPU copy, not the image itself: coherent at `DMA_BUF_IOCTL_SYNC` and (for
  writes) at each commit, so a client that writes with the CPU and renders on the GPU into the same image between two syncs sees one or the other. There is no EGL, no `libgbm` and no `VK_KHR_wayland_surface` yet (all need libwayland / the dynamic loader):
  a client has to render into an exported image and pass it itself, as `tests/linux/waylandvktest.c` does; real toolkits (GTK, Qt) still use `wl_shm`.
  Not run in QEMU yet (only on the CPU emulator and in the host tests, on the software renderer); not tried on virgl or a real GPU.
- **Protocols.** No primary selection, `xdg_activation_v1`, `wp_viewporter`, `wp_fractional_scale_v1`, `wp_cursor_shape_v1`, text input,
  pointer constraints, relative pointer, presentation time, idle inhibit, xdg-foreign, layer shell or tablets. `wl_touch` sends nothing.
- **Clipboard and drag and drop.** `wl_data_device_manager` makes its objects but transfers nothing, and nothing is bridged to the desktop's
  clipboard.
- **Scale and transform.** The output's scale is 1. `set_buffer_scale` and `set_buffer_transform` are checked and then ignored, and buffers are
  drawn 1:1. There is one `wl_output` (the primary monitor), and surfaces only get `enter` for it.
- **Window manager.** Minimizing sends no state. `show_window_menu` does nothing. A popup's `reactive` flag and parent size are ignored, and
  `xdg_wm_base.ping` is never sent.
- **Shared memory semantics.** A file in memory that grows past its block moves (see above). A pool may be mapped past its file's size but within
  its block, where Linux would fault.
- **Windows programs.** Wayland needs descriptor passing, which Windows' `AF_UNIX` lacks, so only Linux programs are clients.
- **Real programs in the OS.** No dynamically linked Wayland program (libwayland in `/Linux`) has been run inside the OS yet. The real-library tests
  run on the development machine against the same compositor code, and the in-OS test uses the raw protocol.
