# LLM and ImGui Integration

Japanese version: [llm-imgui-integration.ja.md](llm-imgui-integration.ja.md)

This document describes how ChatLookDev connects an in-process llama.cpp runtime to the ImGui UI in C++.

## Overview

ChatLookDev keeps the LLM runtime and the UI deliberately separate.

- `ChatLookDevApp` owns ImGui, project state, renderer-facing state, chat transcript, and AI action history.
- `LocalLlmService` owns llama.cpp model/context lifetime and runs model load/generation on a worker thread. Sampler chains are created per generation so grammar state does not leak across replies.
- The worker thread never touches ImGui, D3D12 renderer state, or project data directly.
- Communication back to the UI uses a mutex-protected `std::deque<LocalLlmEvent>`.

The main loop calls `DrainLlmEvents()` before drawing the UI each frame. That keeps all state mutation caused by AI responses on the UI thread.

## UI Flow

`DrawAiChatPanel()` renders the AI Chat panel:

- Model controls: browse GGUF, load model, cancel generation, unload model.
- Runtime controls: context tokens, max reply tokens, CPU threads, GPU layers, structured JSON, temperature, top-p, and the current CPU/GPU inference mode.
- Transcript: user, assistant, and system messages.
- Prompt input: `InputTextMultiline`, with `Send` and `Clear` buttons. `Ctrl+Enter` also sends while the input field is active.
- Action History: latest AI exchanges, raw response, extracted JSON, action results, token counts, timing, and finish reason.

When the user presses `Send`, `SendChatPrompt()` trims the input, builds messages with `BuildLlmMessages()`, submits them to `LocalLlmService::Submit()`, appends the user message to the transcript, and stores the prompt in `m_pendingUserPrompt` so the eventual response can be logged as one exchange.

`Clear` only clears session UI state: transcript, action history, pending prompt, input buffer, and latest AI diagnostics. It does not unload the model or change the scene.

## LLM Service Flow

`LocalLlmService` exposes a small async API:

- `LoadAsync(config)` stops any existing model, validates the GGUF path, and starts `LoadWorker`.
- `Submit(messages)` starts `GenerateWorker` only when the service is `Ready`.
- `CancelGeneration()` requests the current generation loop to stop while keeping the loaded model.
- `Stop()` joins the worker, releases model/context, and moves to `Stopped`.
- `DrainEvents()` returns and clears queued status/response/error events.

`LoadWorker()` creates:

- `llama_model` from the configured GGUF path.
- `llama_context` with configured context size and CPU thread count.
- a short-lived sampler probe to verify that the requested sampler/grammar configuration can initialize.

`Generate()` creates a fresh `llama_sampler` chain, clears context memory, applies the model chat template, tokenizes the prompt, decodes prompt tokens in chunks no larger than `llama_n_batch(context)`, then samples up to `maxTokens`. Chunked prompt decode keeps large scenes from sending an oversized prompt batch into llama.cpp.

- raw generated text
- prompt token count
- output token count
- elapsed milliseconds
- finish reason
- whether grammar was active
- sampler diagnostics

## Structured JSON Generation

When `LocalLlmConfig::structuredJson` is true, `CreateSamplerChain()` tries to add a GBNF grammar sampler with `llama_sampler_init_grammar()`.

The grammar constrains the top-level response to:

```json
{
  "reply": "...",
  "actions": [
    { "method": "set_view_settings", "params": {} }
  ]
}
```

Allowed methods are:

- `set_view_settings`
- `set_environment_settings`
- `set_sun_settings`
- `set_shadow_settings`
- `set_material_preview`
- `set_camera`
- `set_model_transform`

`set_camera` accepts direct numeric camera fields (`target`, `yaw`, `pitch`, `yawDegrees`, `pitchDegrees`, `distance`, and `fovDegrees`) plus view workflow commands: `frameScene`, `preset` (`Front`, `Back`, `Left`, `Right`, `Top`, `Bottom`, `Iso`), `storeBookmark` (`1..3`), and `recallBookmark` (`1..3`). Bookmark recall is rejected if the requested slot is empty.

If grammar initialization fails, the service falls back to unconstrained generation and emits diagnostics. Safety still depends on the C++ validation path, not the grammar alone.

## Response Handling and Actions

`DrainLlmEvents()` routes response events to `HandleLlmResponse()`.

`HandleLlmResponse()` performs this pipeline:

1. Store raw response and runtime stats in an `AiExchange`.
2. Extract the first complete JSON object from the generated text.
3. Parse the JSON with `JsonParser`.
4. Require a top-level object and a string `reply`.
5. Treat missing `actions` as no actions.
6. Require every action to be an object with a supported `method` and object `params`.
7. Validate every action with `ApplyAiAction(..., commit=false)`.
8. If all actions validate, apply them with `ApplyAiAction(..., commit=true)`.
9. Append assistant reply, diagnostics, and action log to the UI.

This two-pass flow prevents partial state changes. If any action is malformed or unsupported, the entire response is rejected and scene/material/camera/view state is not changed.

## Threading Rules

The integration follows a strict ownership rule:

- Worker thread: llama.cpp model/context, per-generation sampler chain, and event production.
- UI thread: ImGui, project state, renderer state, chat transcript, and action application.

The worker thread communicates only through `LocalLlmEvent`. `m_stopRequested` is atomic, and event/status/model pointers are protected by `m_mutex`.

This keeps ImGui and D3D12 calls off the worker thread and avoids cross-thread renderer mutation.

## Persistence

Project JSON stores LLM runtime settings such as model path, context tokens, max tokens, GPU layers, CPU threads, sampling settings, and `structuredJson`. Camera bookmark slots are saved with the project state, while chat history and Action History are not.

Action History is session-only. It is intentionally not serialized into project files.

## Prompt State Size

`BuildControlStateJson()` sends compact renderer state to the LLM. Large scenes can contain many materials, so the prompt includes `materialCount`, the selected material name, and a bounded `materialsPreview` list instead of serializing every material. The selected material is always included in that preview.

## Notes

- ChatLookDev uses llama.cpp in-process, not `llama-server`.
- There is no MCP bridge, runtime shader compiler, or shader editing path in this integration.
- Grammar is used as a generation constraint. Final authority remains C++ validation before any action is applied.
