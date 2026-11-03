# win32k: composition and the "other" services

Files: `src/Process/Win32k/Win32kComposition*.{cpp,hpp}`, test `tests/Win32kCompositionTest.cpp`
(`tests/run_host_tests.sh Win32kCompositionTest`, ASan + UBSan).

This module implements the services of `scripts/win32k-services.txt` whose names do not start with `NtGdi`, `NtUser` or `NtDxgk`
(214 services): DirectComposition, composition surfaces and input sinks, flip objects, the token manager, modern-core window targets, the raw
input manager (RIM), the minuser input layer (MIT/Min) and a few small services. 165 are implemented, 49 are deliberately left unregistered (they keep
answering `STATUS_NOT_IMPLEMENTED`, see the end).

## A note on fidelity

Microsoft does not document these services, and no Windows binary that calls them is in this repository (there is no `dcomp.dll`, `dwm.exe` or
`user32` raw-input client here). The argument lists, structure layouts and command encodings below are this module's model of them, reconstructed from public
reverse-engineering notes and from what the objects have to do; they are **not** verified against a real Windows user mode. What is faithful is the
object model (what is created, shared, closed and cleaned up), the status codes for bad handles, rights, sizes and pointers, and the queue semantics.
Where a service needs a compositor or hardware this system does not have, it answers what a system without it answers (`STATUS_NOT_SUPPORTED`,
`STATUS_ACCESS_DENIED`) and says so here. Arguments are numbered from 0 as in `Call::arg()`.

## Object model

Every object a handle can name derives from `Object` (a `Kind` tag, no virtual functions) and is held by a `SharedPointer<Object>` in the module's
`HandleTable`:

* a handle is `0x40000000 | generation << 20 | (index + 1) << 2`; the generation changes whenever a slot is reused, so a stale handle never names the new
  object;
* each entry has an owner process and an access mask (`kRightQuery` 1, `kRightModify` 2, `kRightAll` 0x1F0003; generic rights are mapped, unknown bits are
  refused); a lookup checks owner, kind (`STATUS_OBJECT_TYPE_MISMATCH`) and rights (`STATUS_ACCESS_DENIED`);
* objects shared between processes are shared by handle duplication (`NtDCompositionDuplicateHandleToProcess`, tokens): the object lives until its
  last handle goes; access can only shrink;
* limits (hostile callers): 4096 handles per process, 65536 in all, and per-kind caps (256 channels, 4096 surfaces, ...) answering `STATUS_QUOTA_EXCEEDED`;
* when a process ends, `on_process_exit` closes everything it owned (the objects survive while another process still holds a handle), clears the DWM role,
  the mouse intercept, its minuser threads, input spaces, navigation windows and per-process settings.

Real NT objects are used where Windows uses them: the sections and events these services hand out (`CreateAndBindSharedSection`, the surface section and
events, the flip-object events) are ordinary descriptors of the process table (`DescriptorKind::Section` / `Event`), closed with `NtClose`, waited for
with `NtWaitForSingleObject`; events given by the caller (commit completion event ...) are referenced by the module and stay valid after the caller closes
its handle. The module's own handles (channels, surfaces, sinks ...) are **not** in the process descriptor table: they are destroyed by the service that
made them (`NtDCompositionDestroyChannel`, `NtCloseCompositionInputSink`, ...) or by process exit; `NtClose` on them answers `STATUS_INVALID_HANDLE`.
(Making `NtClose` call back into the module needs a hook in `WindowsObjects.cpp`, outside this module; see "Contract" below.)

### Locking

Handlers are registered with `locked = false` and wrapped by `thunk<>`, which takes the system lock itself because most services need `ObjectManager` calls
(set an event, add a reference, make a section) and those take the lock, which is not recursive. A body that needs one gives the lock up with
`Ctx::Unlocked` and looks again at what it needs afterwards (objects it keeps across such a stretch are pinned by a `SharedPointer`). Events are never
touched under the lock: an object that lets go of an event or must signal one queues the operation (`State::defer_event`), and the end of every call runs the
queue in order with the lock given up. Consequence: when a process ends, events that its objects signal (a flip object's *lost* event) are signalled at the
end of the next composition call, not at the instant of the exit.

## DirectComposition (`NtDComposition*`)

A **channel** is the unit of work: a command buffer (page-aligned shared memory, mapped into the creator's address space at creation) plus a table of
resources.

| Service | Behaviour |
|---|---|
| `CreateChannel(PHANDLE, PSIZE_T size in/out, PVOID* base)` | size 0 = 64 KiB, rounded up to pages, at most 4 MiB; maps the buffer into the caller (no address space: `STATUS_NOT_SUPPORTED`); failure to write an output undoes everything |
| `CreateDwmChannel(...)` | the same for the compositor; the first process to ask owns the DWM role until its last DWM channel is destroyed or it exits; others get `STATUS_ACCESS_DENIED` |
| `DestroyChannel(h)` | unmaps the view (creator), drops the handle |
| `ProcessChannelBatchBuffer(h, size, DWORD* consumed, DWORD* commands)` | needs modify; runs the first `size` bytes of the buffer (see below) |
| `CommitChannel(h, flags 0/1, event?, DWORD* batch)` | new batch id (1, 2, ...); resources released since the last commit and no longer referenced are deleted and their ids queued for `GetDeletedResources`; a frame id is allocated and recorded per batch; signals the channel's completion event and the event argument; wakes `WaitForChannel` |
| `SubmitDWMBatch(h, size, ...)` | processes and commits a DWM channel's buffer (non-DWM channel: access denied) |
| `GetBatchId`, `GetFrameId`, `GetFrameIdFromBatchId` | last committed batch; global frame counter; the frame of a batch (`STATUS_NOT_FOUND` after 256 batches) |
| `GetDeletedResources(h, DWORD* ids, DWORD* count in/out)` | removes up to `count` ids; `STATUS_BUFFER_OVERFLOW` when more wait |
| `ReleaseAllResources`, `SetChannelCommitCompletionEvent`, `SetChannelConnectionId` | as named; the connection id must be one of the caller's connections |
| `WaitForChannel(h, timeout ms, flags 0/1)`, `SyncWait(sync, timeout)`, `WaitForCompositorClock(timeout)` | real waits (`wait_until`): a commit is reported once; a synchronization object counts commits; the clock ticks at 60 Hz of the monotonic clock |
| `Synchronize(h, flags, timeout)` | the compositor takes a batch at its commit, so there is nothing to wait for: validates and returns |
| `CreateConnection(PHANDLE, UINT64* id)`, `DestroyConnection`, `GetConnectionBatch` | a connection counts the commits of the channels attached to it; `ConnectPipe` answers `STATUS_NOT_SUPPORTED` (no compositor process to remote to) |
| `CreateSynchronizationObject`, `CommitSynchronizationObject(channel, sync, 0)` | |
| `CreateSharedResourceHandle`, `ReferenceSharedResourceOnDwmChannel(shared, dwm channel, id)` | the shared resource becomes resource `id` of the DWM channel, once |
| `CreateAndBindSharedSection(channel, id, size, PHANDLE)` | an ordinary section handle bound to a new resource `id` |
| `DuplicateHandleToProcess(h, process, PHANDLE out, access, options)` | any handle of the module into another process's table; options 2 = same access; access may only shrink; ended target: `STATUS_PROCESS_IS_TERMINATING` |
| `DuplicateSwapchainHandleToDwm(surface, PHANDLE)` | a handle for the DWM process (`STATUS_NOT_SUPPORTED` when there is none) |
| `GetChannels(HANDLE* out, DWORD* count)` | DWM only: handles to the other processes' channels, once each |
| `BeginFrame/ConfirmFrame/RetireFrame/DiscardFrame`, `GetFrameLegacyTokens`, `GetFrameSurfaceUpdates` | the compositor's frame state machine (begun -> confirmed -> retired, or discarded); no legacy tokens or surface updates exist |
| `GetStatistics`, `GetFrameStatistics` | counters (struct sizes checked) |
| `BoostCompositorClock`, `EnableMMCSS`, `SuspendAnimations`, `SetDebugCounter` (DWM), `Set/GetMaterialProperty` | recorded state (material properties per process) |
| `Telemetry*` | scenario references and touch interactions are tracked with Windows' error behaviour (unbalanced unreference, double begin); nothing consumes them |

### The command buffer

The real format is not public. This module defines: every command is `{u32 opcode, u32 length}` (length counts the header, a multiple of 4, at most
4104) followed by its payload. Opcodes: 0 Nop; 1 CreateResource `{id, type}`; 2 ReleaseResource `{id}`; 3 ReleaseAllResources; 4 SetProperty
`{id, property, value (up to 8 bytes)}`; 5 AddChild `{parent, child}`; 6 RemoveChild. A resource that other resources refer to stays alive after its release
until the referrers go; deletion happens at the next commit. The batch stops at the first bad command (unknown opcode, wrong length, missing or duplicate
resource, a table over 4096 resources / 16384 properties / 16384 edges): `STATUS_INVALID_PARAMETER`, with `consumed` and `commands` telling how far it got.
Framing is validated against the real buffer bounds before any payload is read.

## Composition surfaces, input sinks, tokens (`Win32kCompositionSurface.cpp`)

* `CreateCompositionSurfaceHandle(PHANDLE, access)`, `ValidateCompositionSurfaceHandle`, `Bind/UnBindCompositionSurface(surface, channel, resource)` (the
  resource must exist and not be released), `QueryCompositionSurfaceBinding`.
* `Set/QueryCompositionSurfaceStatistics` (24-byte `{size, flags, rate num, rate den, present count}`; a frame rate needs both parts),
  `QueryCompositionSurfaceFrameRate`, `NotifyPresentToCompositionSurface(surface, id, RECT*, 0)` (counts presents, remembers the dirty rectangle, makes the
  surface "realized" in software (2), signals its present event), `OpenCompositionSurfaceDirtyRegion` (takes the rectangle), `OpenCompositionSurfaceRealizationInfo`,
  `QueryCompositionSurfaceRenderingRealization`.
* `SetCompositionSurfaceBufferUsage(surface, usage, section size)` defines the backing section; `OpenCompositionSurfaceSectionInfo` and
  `TokenManagerOpenSectionAndEvents(token, PHANDLE section, ready event, present event)` give the caller real section / event handles (made on first use, one
  surface section and two events per surface).
* `SetCompositionSurfaceDirectFlipState` (0..2), `SetCompositionSurfaceIndependentFlipInfo` / `ConfirmCompositionSurfaceIndependentFlipEntry`,
  `SetCompositionSurfaceAnalogExclusive` (1 answers `STATUS_NOT_SUPPORTED`: no analog output).
* Input sinks: `Create(Implicit)CompositionInputSink(PHANDLE, luid, view id)`, `Close`, `Duplicate`, `QueryCompositionInputSink` (32-byte info),
  `...IsImplicit`, `...SinkLuid`, `...SinkViewId`, `...QueueAndTransform`, `UpdateInputSinkTransforms` (2x3 float matrix; NaN / infinity refused),
  `CompositionSetDropTarget`.
* Tokens: `TokenManagerCreateFlipObjectTokenHandle` / `...ReturnTokenHandle(endpoint, PHANDLE)`, `TokenManagerCreateCompositionTokenHandle(surface, PHANDLE)`;
  a token is duplicated into another process, which opens what it names. The analog-exclusive services (`GetAnalogExclusiveSurfaceUpdates`,
  `GetAnalogExclusiveTokenEvent`, `ConfirmOutstandingAnalogToken`) answer `STATUS_NOT_SUPPORTED`.

## Flip objects (`NtFlipObject*`, `Win32kCompositionFlip.cpp`)

A flip object is a present queue between one *producer* and one *consumer* endpoint, each its own handle kind.

* `Create(PHANDLE producer, access, flags 0)`; a token for it goes to the consumer's process, which calls `Open(token, access, PHANDLE consumer)`. A Flip token
  gives the consumer end (a second consumer: `STATUS_DEVICE_BUSY`; producer gone: `STATUS_PORT_DISCONNECTED`), a return token gives the producer end back.
* Producer: `AddPoolBuffer(h, optional section handle, size, DWORD* id)` (up to 64; the section is referenced), `RemovePoolBuffer` (busy while a present or the
  consumer's usage reference holds it), `AddContent(h, buffer, flags, DWORD* id)`, `RemoveContent`, `SetContent(h, content, present id, flags)` queues a present
  (ids unique, 256 at most), `PresentCancel` (only before the consumer took it), `EnablePresentStatisticsType(type 0..7)`,
  `SetMaximumBackchannelQueueDepth(1..256)`, `QueryNextMessageToProducer` / `ReadNextMessageToProducer` (`{DWORD type, data}`; too small a buffer leaves
  the message queued).
* Consumer: `AcquirePresent(h, {present id, content, buffer, flags}*)` takes the oldest queued present (`STATUS_NO_MORE_ENTRIES` if none),
  `BeginProcessPresent`, `EndProcessPresent(h, id, status)` (the buffer goes back; with statistics enabled a type 0x100 message `{id, status, completed count}` reaches the
  producer), `PostMessage(h, type, data, size <= 56)` (`STATUS_QUOTA_EXCEEDED` at the queue depth), `QueryBufferInfo`, `AdjustUsageReference(h, buffer, delta)`.
* Events (ordinary event handles, one new manual-reset event per query): `QueryBufferAvailableEvent` is signalled while some pool buffer is free of presents and
  reset while none is; `QueryLostEvent` (either end) is signalled while the other end is gone and rearmed when one attaches. `DisconnectEndpoint`, closing the
  last handle or process exit of an end signal the peer's lost events. `QueryEndpointConnected` reports whether the other end is there.

## Raw input manager (`NtRIM*`, `Win32kCompositionRim.cpp`)

The machine has two virtual devices: 1 = keyboard, 2 = mouse (properties, an ACPI-style device path, no HID preparsed data
(`STATUS_INVALID_DEVICE_REQUEST`), no IOCTLs). A registration (`RegisterForInput(PHANDLE, device mask, flags, hwnd)`; `Ex` takes a usage filter that must be 0)
receives a queue of 40-byte `RimPacket`s for the devices in its mask; an observer (`AddInputObserver`) sees the same stream without consuming anything for
others and can be paused (`UpdateInputObserverRegistration(h, 0, flags)`). `ReadInput(reg, buffer, size, DWORD* count, timeout ms)` reads as many whole packets as
fit (0 timeout: `STATUS_NO_MORE_ENTRIES` when empty; otherwise a real wait woken by new input); queues hold 256 packets, the oldest is dropped and the next
packet read carries flag bit 0. `ObserveNextInput(observer, buffer, size, timeout)` is the same for one packet. The kernel's keyboard and mouse drivers feed
the manager with `composition::rim_feed_key` / `rim_feed_mouse` (public in `Win32kComposition.hpp`); the MIT synthesize services do the same.

## Minuser input (`NtMIT*`, `NtMin*`) and synthesis

* Thread state: `MITInitMinuserThread(0)` / `Uninit`, `ActivateInputProcessing` / `Deactivate`, `MinQSuspendInputProcessing(bool)` (nests),
  `MinQUpdateWakeMask`, `MITMinuserSetInputTransformOffset` / `MinGetInputTransform`, `MITMinuserWindowCreated/Destroyed(hwnd)` — per thread, errors as
  `STATUS_INVALID_DEVICE_STATE`.
* `MITSynthesizeKeyboardInput(vk, scan, flags)` (flags: 1 extended, 2 up, 4 unicode, 8 scan code) and `MITSynthesizeMouseInput(dx, dy, data, MOUSEEVENTF flags)`
  validate their arguments (key range, X-button number, absolute 0..65535, no overflow of relative moves), feed the raw input manager and pass the input on to
  the window manager through `InputSink` (`user::inject_key/char/mouse_*` and `cursor_position` when the window manager is linked in; tests install their
  own). The window manager's `inject_*` take the system lock themselves, so they are called with it given up.
  `MITEnableMouseIntercept` / `Disable`: one process at a time diverts other processes' synthesized mouse input away from the window manager (the raw input
  manager still gets it). `MITSynthesizeTouchInput` answers `STATUS_NOT_SUPPORTED`.
* `MITPostWindowEventMessage`, `MITPostMouseInputMessage` (mouse messages only), `MITPostThreadEventMessage` -> `user::post_message` /
  `post_thread_message` (`STATUS_INVALID_WINDOW_HANDLE` when the window manager refuses).

## Input spaces and small services

`ConfigureInputSpace(id, RECT*, flags)` (null rectangle removes), `SetCursorInputSpace`, `SetPointerDeviceInputSpace(device, id)`, `InputSpaceRegionFromPoint`,
`MapVisualRelativePoints(from, to, POINT*, count)` (64-bit arithmetic, result must fit 32 bits), `Enable/IsOneCoreTransformMode`, `SetShellCursorState`,
`HWCursorUpdatePointer` (`STATUS_NOT_SUPPORTED`: no hardware cursor plane).

Modern core: `Create/DestroyDCompositionHwndTarget`, `Create/DestroyGDIHwndTarget(hwnd, PHANDLE)` (one of each kind per window; the hwnd is treated as an
opaque id, the window manager is not asked), `GetWindowContentVisual`, `GetNavigationWindowVisual` (only for registered navigation windows),
`Register(Enhanced)/UnregisterNavigationWindowHandle`, `SetNavigationServiceSid` (SID structure validated), `Enable/IsResizeLayoutSynchronization...`,
`GetResizeDCompositionSynchronizationObject` (a handle to a per-window synchronization object, only while enabled).

## Left unregistered (answer `STATUS_NOT_IMPLEMENTED`)

* needs a DWM / compositor / hardware this system lacks, or semantics that could only be invented: `DCompositionAddCrossDeviceVisualChild`,
  `RemoveCrossDeviceVisualChild`, `SetChildRootVisual`, `SetBlurredWallpaperSurface`, `NotifySuperWetInkWork`, `RegisterThumbnailVisual`,
  `RegisterVirtualDesktopVisual`, `UpdatePointerCapture`, `GetTargetStatistics`, `OpenCompositionSurfaceSwapChainHandleInfo`,
  `QueryCompositionSurfaceHDRMetaData`, `Compositor*` notifications (`NotifyExitWindows`, `NotifyForegroundChanged`, `UpdateLastInputTime`),
  `ModerncoreBeginLayoutUpdate`, `ModerncoreProcessConnect`;
* service threads that never return (`CompositionInputThread`, `TokenManagerThread`, `ModerncoreIdleTimerThread`) and the PnP/timer callbacks of the
  raw input manager (`RIMOnPnpNotification`, `RIMOnAsyncPnpWorkNotification`, `RIMOnTimerNotification`): they belong to processes that only exist with a real
  DWM / PnP manager;
* minuser and core-messaging internals with no observable effect to model (`MITSetInputCallbacks`, `SetInputDelegationMode`, `SetKeyboardInputRoutingPolicy`,
  `SetKeyboardOverriderState`, `SetLastInputRecipient`, `SetForegroundRoutingInfo`, `SetInputObservationState`, `UpdateInputGlobals`,
  `ProcessDelegateCapturedPointers`, `PrepareSend/ReceiveInputMessage`, `CoreMsgKOpenConnectionTo`, `DispatchCompletion`, `AccessibilityTimerNotification`,
  `ConfigureVirtualTouchpad`, `GetCursorUpdateHandle`, `MinInteropCoreMessagingWithInput`, `MinQPeekForInput`) and `KSTInitialize` / `KSTWait`;
* `DesktopCaptureBits`, `VisualCaptureBits` (depend on the unspecified GDI screen capture layout), `RIMFreeInputBuffer` (this module copies packets into the caller's
  buffer), `RIMGetPhysicalDeviceRect`, `RIMGetSourceProcessId`, `RIMSetDeadzoneRotation`, `RIMSetExtendedDeviceProperty`.

`tests/Win32kCompositionTest.cpp` (`test_coverage`) pins this list: every service of the module is either registered or named above.

## Contract for the other modules

* `composition::set_input_sink` / `default_input_sink`: the MIT services call the window manager only through this table, filled from `user::inject_key`,
  `inject_char`, `inject_mouse_move/button/wheel`, `post_message`, `post_thread_message`, `cursor_position` and `gdi::screen_width/height` as **weak**
  references: the module links without them and the services that need one answer `STATUS_NOT_SUPPORTED`.
* `composition::rim_feed_key` / `rim_feed_mouse` are for the kernel's input drivers (take the system lock themselves).
* A process-exit hook is registered; nothing else in other modules changes.
* Suggested follow-ups outside this module: (1) a hook so `NtClose` reaches the module for its handles; (2) an optional "after the lock is released" stage in
  `process_exited` so events of ending processes are signalled at once.
