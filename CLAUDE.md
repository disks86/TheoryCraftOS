# TheoryCraftOS

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

## Filesystem layout

The system filesystem has a fixed layout: `/` is the primary partition, `/EFI` the EFI partition, `/Desktop` (with `Downloads`, `Documents`,
`Pictures`, `Video` and `Audio`) the user's files, `/Windows` and `/Linux` the roots of Windows and Linux processes, and `/AppStore/<App>` the
files each application keeps. See [docs/FileSystemLayout.md](docs/FileSystemLayout.md) and `src/FileSystem/SystemLayout.hpp`.

- Use the constants and helpers in `filesystem::layout` for these places; do not spell the paths out.
- Browser downloads go to `/Desktop/Downloads`, documents to `/Desktop/Documents`, and so on. An application keeps its own files (cache,
  settings, state) under `/AppStore/<Application>/` and nowhere else.
- Do not add top-level folders to `/`. If something does not fit, extend the layout doc and `SystemLayout.hpp` in the same change.

## Delegating routine work to a local model

When possible, use Ollama with the `qwen3-coder-next` model (the `mcp__ollama__*` tools, e.g. `generate` or `chat`) for routine work:

- explanations of existing code
- test generation
- documentation (comments, README entries)
- simple refactoring

Give the model the relevant code in the prompt, since it cannot read the repository. Review what it returns before using it: check it against the project's conventions (containers over raw pointers, the license header, the surrounding style), then build and run the tests.

Do the work directly instead when the task needs design judgement, touches several files at once, or involves kernel, hardware, or concurrency details. Also do it directly if Ollama or the model is unavailable.

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
- Ask questions only for genuinely blocking ambiguity, destructive operations, or decisions with significant architectural consequences.
- Never claim success without verifying the result.
- Before stopping, review the diff, run relevant tests, and list any incomplete work and its cause.
- Once verified, commit the changes to the main branch.

## Completion Criteria

A task is complete only when the requested functionality is implemented, integrated, and validated to the extent possible in the current environment.