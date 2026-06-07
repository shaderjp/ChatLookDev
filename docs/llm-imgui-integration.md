# LLM and ImGui Integration

Japanese version: [llm-imgui-integration.ja.md](llm-imgui-integration.ja.md)

This document describes how ChatLookDev connects an in-process llama.cpp runtime to the ImGui UI in C++.

## Overview

ChatLookDev keeps the LLM runtime and the UI deliberately separate.

- `ChatLookDevApp` owns ImGui, project state, renderer-facing state, chat transcript, and AI action history.
- `LocalLlmService` owns llama.cpp model/context/sampler lifetime and runs model load/generation on a worker thread.
- The worker thread never touches ImGui, D3D12 renderer state, or project data directly.
- Communication back to the UI uses a mutex-protected `std::deque<LocalLlmEvent>`.

The main loop calls `DrainLlmEvents()` before drawing the UI each frame. That keeps all state mutation caused by AI responses on the UI thread.

## UI Flow

`DrawAiChatPanel()` renders the AI Chat panel:

- Model controls: browse GGUF, load model, cancel generation, unload model.
- Runtime controls: context tokens, max reply tokens, CPU threads, GPU layers, structured JSON, temperature, top-p.
- Transcript: user, assistant, and system messages.
- Prompt input: `InputTextMultiline`, with `Send` and `Clear` buttons.
- Action History: latest AI exchanges, raw response, extracted JSON, action results, token counts, timing, and finish reason.

When the user presses `Send`, `SendChatPrompt()` trims the input, builds messages with `BuildLlmMessages()`, submits them to `LocalLlmService::Submit()`, appends the user message to the transcript, and stores the prompt in `m_pendingUserPrompt` so the eventual response can be logged as one exchange.

`Clear` only clears session UI state: transcript, action history, pending prompt, input buffer, and latest AI diagnostics. It does not unload the model or change the scene.

## LLM Service Flow

`LocalLlmService` exposes a small async API:

- `LoadAsync(config)` stops any existing model, validates the GGUF path, and starts `LoadWorker`.
- `Submit(messages)` starts `GenerateWorker` only when the service is `Ready`.
- `CancelGeneration()` requests the current generation loop to stop while keeping the loaded model.
- `Stop()` joins the worker, releases model/context/sampler, and moves to `Stopped`.
- `DrainEvents()` returns and clears queued status/response/error events.

`LoadWorker()` creates:

- `llama_model` from the configured GGUF path.
- `llama_context` with configured context size and CPU thread count.
- `llama_sampler` chain, optionally with structured JSON grammar.

`Generate()` clears context memory, applies the model chat template, tokenizes the prompt, decodes prompt tokens, then samples up to `maxTokens`. The response event includes:

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
- `set_material_preview`
- `set_camera`
- `set_model_transform`

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

- Worker thread: llama.cpp model/context/sampler and event production.
- UI thread: ImGui, project state, renderer state, chat transcript, and action application.

The worker thread communicates only through `LocalLlmEvent`. `m_stopRequested` is atomic, and event/status/model pointers are protected by `m_mutex`.

This keeps ImGui and D3D12 calls off the worker thread and avoids cross-thread renderer mutation.

## Persistence

Project JSON stores LLM runtime settings such as model path, context tokens, max tokens, GPU layers, CPU threads, sampling settings, and `structuredJson`.

Action History is session-only. It is intentionally not serialized into project files.

## Notes

- ChatLookDev uses llama.cpp in-process, not `llama-server`.
- There is no MCP bridge, runtime shader compiler, or shader editing path in this integration.
- Grammar is used as a generation constraint. Final authority remains C++ validation before any action is applied.
