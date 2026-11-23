# TheoryCraftOS

## Goal
The goal of this project is to produce a Computer Operating system with the following key features.
- High stability
- High performance
- High Concurrency
- Mega Kernel (Most common feature built into kernel instead of userland)
- Simple desktop user experience
- Run applications from Linux or Windows by emulating SYSCALL interface and any needed usermode libraries.
- Run applications compiled for different architectures by using a binary recompiler.
- Support a wide range of x86, ARM, and RISC-V hardware.
- Support Hardware accerlators and special instructions whenever possible.

## Navigating the code with graphify

The repository is large, so find code through the graphify knowledge graph (`graphify-out/graph.json`, kept current by the git hooks;
it is not committed) before reading or grepping raw files:

- Start every question about where something lives, how a subsystem works, or what touches what with `graphify query "<question>"`
  (or the graphify MCP tools). Use `graphify explain "<symbol or concept>"` for one node and its neighbours, `graphify path "<A>" "<B>"`
  for how two pieces connect, and `graphify section "<heading>"` to read a section of a doc in `docs/` without opening it.
- Before changing a shared class, interface or syscall, use the graph to find its callers, implementers and tests, so a change to one
  layer is carried through every layer that depends on it (kernel service, user-mode DLL or library, tests).
- Read raw files only for the lines the graph pointed to. Use grep for literal text the graph does not model: string constants,
  error messages, syscall numbers, `.def` ordinals, CMake options, generated files.
- Use `graphify-out/GRAPH_REPORT.md` only for broad architecture questions that query/explain/path do not answer.
- If `graphify-out/` is missing or a result is clearly stale, run `graphify update .` (AST only, no API cost) and query again. Run it
  after a large change too, so the next query sees the new code.
- Put these rules in the prompt of every subagent that explores code.
- Skip the graph when the target is already known: a file and line from a compiler error, a test failure or an earlier read.

## Compatibility layers are implemented to completion

The Windows and Linux userlands (ntdll, kernel32, user32, gdi32, the other system DLLs, Linux syscalls, the vDSO, glibc-facing
behaviour), and the other compatibility surfaces (Direct3D, Vulkan, OpenGL, X11, Wayland, PipeWire, D-Bus, file formats and
protocols), exist so that real programs run unchanged. A half-implemented API is worse than a missing one: the program takes a path its
authors never tested and fails far from the cause.

- Implement each function, syscall, flag, message, interface method or structure field the work touches to its full documented
  behaviour, matching the reference implementation (Windows, Linux, the specification), including its error codes, edge cases,
  out-parameters, `GetLastError`/`errno` values, alignment and structure sizes, and the 32-bit and 64-bit variants.
- Do not leave stubs, `TODO`s, `return STATUS_NOT_IMPLEMENTED`/`-ENOSYS`, or "succeed and do nothing" bodies on purpose to finish
  sooner. A stub that reports success is the worst case: never fake success for something that did not happen.
- If a feature needs a layer below it that does not exist yet (a kernel service, a driver, a syscall), build that layer as part of the
  same task rather than stopping at the boundary.
- When the whole of an API family is too large for one change, finish a coherent part of it completely (every function in it, every
  flag of those functions) instead of touching many functions shallowly.
- Only when something truly cannot be done in this environment (it needs hardware, firmware or a licence that is not available)
  return the real error code that Windows or Linux returns for an unsupported request, add a comment saying why, and list it in the
  final report and in the relevant memory/notes as an open gap. Never hide it.
- Verify against the reference: Wine's conformance tests, the Linux behaviour (LTP, real binaries), or a host test comparing with the
  real implementation, and real programs run under QEMU. "It compiles" or "the program got further" is not done.

## Memory safety

Write new code so that memory errors cannot happen, not so that they are unlikely:

- Own memory with containers and smart pointers: `UniquePointer`/`SharedPointer`, `ByteBuffer`, `String`, and the `ObjectContainer` types, all allocator-backed. Never write `new`, `delete`, `malloc` or `free` in application code. Reuse an existing class before adding one.
- Pass memory around as `ArrayView`/`ConstByteView`, `StringView`, `ByteReader` and `ByteWriter`, not as a pointer plus a length. A raw pointer is acceptable only as a non-owning, non-null reference to a single object, or at a hardware or ABI boundary (MMIO, DMA descriptors, the JS cell layout).
- Use the bounds-checked accessors (`operator[]`, `subview()`, `first()`). Use `try_get()`, `try_subview()` or `ByteReader` where the input is untrusted, and handle failure.
- Prefer range-based `for` loops. Use an index loop only when the index itself is needed (parallel arrays, offsets, reverse or strided walks).
- Check arithmetic on sizes and offsets that come from input before using it (`n > limit - size` instead of `size + n > limit`). Growth loops must not be able to wrap.
- Do not use `const_cast` to write into a buffer you own. Add a writable accessor instead, such as `ByteBuffer::writable_view()`. Do not use C string or memory functions (`strcpy`, `sprintf`, raw `memcpy`); use the view `copy_from`/`fill` methods.
- Keep every `reinterpret_cast` in one small helper with a comment saying why it is safe (alignment, lifetime), and don't scatter casts through logic code.
- New decoders and parsers need truncation and bit-flip cases in their host tests. Run them with `tests/run_host_tests.sh <TestName>`, which builds with ASan and UBSan, and fix any sanitizer report.

## Error handling

Report errors through return values, not exceptions, wherever the requirements allow it:

- Return a `bool`, a status or error enum, an `Optional<T>` (`Utility/Optional.hpp`) or a small result struct, and have the caller
  check it. Mark such functions `[[nodiscard]]` so an ignored failure is a compiler warning.
- Kernel-mode code must never throw. The kernel is built with `-fno-exceptions` and has no unwinder, so `throw`, `try`/`catch` and
  anything that relies on them (library calls that throw, a throwing `operator new`) are not allowed there. Handle the failure, pass it
  up, or, if the state is unrecoverable, panic.
- Use exceptions only in user-mode code where a requirement demands them, such as an emulated ABI or third-party code whose contract is
  defined in terms of them. Keep them inside that boundary and turn them into return values before they reach kernel code.

## Concurrent data structures

Shared state touched by more than one core, thread or interrupt handler goes in one of these, not in a bare container, a hand-rolled
atomic dance, or a raw `SpinLock` wrapped around ad-hoc fields. Pick the lightest one that fits:

- `Hive<T>` (`ObjectContainer/Hive.hpp`): the container for objects that other code holds pointers to (tasks, connections, buffers,
  descriptors). O(1) insert and erase, and elements never move, so a pointer stays valid until that element is erased. Prefer it over
  `List`/`Array` plus indices whenever references must survive other insertions and removals.
- `Concurrent*` containers (`ObjectContainer/Concurrent/`: queue, stack, list, dictionary, priority queue, ...): the default for shared
  collections with short operations. They are `Synchronized` wrappers, so every access takes a `SpinLock`: never call back into the same
  container from inside `with()`/`for_each()`, and copy values out instead of keeping references past the lock.
- `Synchronized<T>` / `AtomicSharedPointer<T>` (`Synchronization/`): a single shared value or a swappable `SharedPointer` that readers load
  a private copy of. Use these before writing a new lock-protected class.
- RCU (`Kernel/Rcu.hpp`, shaped after C++26 `<rcu>`): read-mostly data that is read far more often than it changes (routing and lookup
  tables, registries, configuration). Readers use `RcuReader` and never block or write; the writer publishes a new version with
  `rcu_assign_pointer`, retires the old one (`RcuObjectBase`, `rcu_retire`) and it is freed after a grace period. Readers must not sleep
  inside the section. Use `rcu_dereference` on the published pointer.
- Hazard pointers (`Kernel/HazardPointer.hpp`): safe reclamation for lock-free structures where readers follow pointers to nodes that
  another core may unlink (`HazardDomain`, `HazardGuard::protect`, `HazardDomain::retire`). Use them when readers must hold a few
  individual nodes for a short time, or when a reader may stall (preemption, interrupt) and must not hold back all reclamation, which RCU
  would. Keep guards short-lived: slots are fixed in number.

Guidance:

- Do not free (or reuse) an object that a lock-free reader might still see. Retire it through RCU or a hazard domain; never `delete`
  it right after unlinking.
- Both RCU and hazard pointers work from threads, interrupt handlers and early boot, with no allocation and no interrupt masking, so use
  them on paths that must not take a `SpinLock`.
- Do not reach for lock-free code when a `Concurrent*` container is fast enough. Add a lock-free structure only where profiling or a
  context (interrupt handler, hot read path) demands it, and keep the memory-safety rules above.
- New concurrent code needs a host test that exercises several threads (see `tests/HiveTest.cpp`, `tests/RcuTest.cpp`,
  `tests/HazardPointerTest.cpp`; `src/ConcurrentDemo.cpp` shows the containers in use). Run it with `tests/run_host_tests.sh <TestName>`.

## Filesystem layout

The system filesystem has a fixed layout: `/` is the primary partition, `/EFI` the EFI partition, `/Desktop` (with `Downloads`, `Documents`,
`Pictures`, `Video` and `Audio`) the user's files, `/Windows` and `/Linux` the roots of Windows and Linux processes, and `/AppStore/<App>` the
files each application keeps. See [docs/FileSystemLayout.md](docs/FileSystemLayout.md) and `src/FileSystem/SystemLayout.hpp`.

- Use the constants and helpers in `filesystem::layout` for these places; do not spell the paths out.
- Browser downloads go to `/Desktop/Downloads`, documents to `/Desktop/Documents`, and so on. An application keeps its own files (cache,
  settings, state) under `/AppStore/<Application>/` and nowhere else.
- Do not add top-level folders to `/`. If something does not fit, extend the layout doc and `SystemLayout.hpp` in the same change.

## User interfaces

Every user interface (the desktop shell, the built-in applications, dialogs, new toolkit controls) follows
[docs/UserInterfaceGuidelines.md](docs/UserInterfaceGuidelines.md). Read the sections that apply before writing or changing interface code.

- Everything works from the keyboard alone, and every basic task with single clicks. Standard shortcuts keep their standard meanings.
- Reuse the controls and dialogs in `src/Gui` and `src/Desktop` before adding one. Colours come from `gui::Theme`, sizes follow the font and the display scale.
- Disable what does not apply, make actions undoable or confirm them, and never make a destructive action the default button.
- Run the review checklist at the end of that document before calling an interface change finished.

## Generating images and audio

Use ComfyUI (the `mcp__comfyui__*` tools, e.g. `generate_image`, or `create_workflow` and `enqueue_workflow` for audio) to generate any images or audio the work needs, such as test assets, sample media, and icons.

ComfyUI output defaults to `/mnt/shared`. Look there for generated files, and copy only the ones the project needs into the repository (for example under `tests/data`). Generation is asynchronous, so poll the queue or history until the job finishes before using the result.

## Autonomous Implementation Policy

- For substantial tasks, own the implementation from investigation through verification.
- Do not stop after producing a plan. Start implementing immediately after sufficient investigation.
- Do not ask for approval between routine steps.
- Break large tasks into internal milestones and continue through them autonomously.
- Use subagents when they can independently investigate or review separate parts of the task.
- Build and test after meaningful changes and fix failures before proceeding.
- If a test fails, investigate the underlying cause rather than merely reporting the failure.
- Keep a short progress checklist and update it as milestones are completed.
- Make reasonable, reversible decisions without asking me to choose between minor implementation details.
- Do not stop at a partial implementation of a compatibility API (see "Compatibility layers are implemented to completion").
- Ask questions only for genuinely blocking ambiguity, destructive operations, or decisions with significant architectural consequences.
- Never claim success without verifying the result.
- Before stopping, review the diff, run relevant tests, and list any incomplete work and its cause.
- Once completed, automatically commit the changes to the main branch.

## Completion Criteria

A task is complete only when the requested functionality is implemented, integrated, and validated to the extent possible in the current environment.
