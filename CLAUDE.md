# TheoryCraftOS

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
