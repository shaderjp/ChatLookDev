# LLM と ImGui の接続

English version: [llm-imgui-integration.md](llm-imgui-integration.md)

このドキュメントでは、ChatLookDev が C++ 上で in-process の llama.cpp ランタイムと ImGui UI をどう接続しているかを説明します。

## 概要

ChatLookDev では、LLM ランタイムと UI を意図的に分離しています。

- `ChatLookDevApp` は ImGui、project state、renderer に渡す state、chat transcript、AI action history を持ちます。
- `LocalLlmService` は llama.cpp の model/context の lifetime を持ち、model load と generation を worker thread で実行します。sampler chain は generation ごとに作成し、grammar state が応答間で漏れないようにしています。
- worker thread は ImGui、D3D12 renderer state、project data を直接触りません。
- UI への通知は mutex で保護された `std::deque<LocalLlmEvent>` を通します。

main loop は毎フレーム UI 描画前に `DrainLlmEvents()` を呼びます。これにより、AI response による state 変更はすべて UI thread 上で行われます。

## UI の流れ

`DrawAiChatPanel()` が AI Chat パネルを描画します。

- Model controls: GGUF の参照、model load、generation cancel、model unload。
- Runtime controls: context tokens、max reply tokens、CPU threads、GPU layers、structured JSON、temperature、top-p、現在の CPU/GPU inference mode。
- Transcript: user、assistant、system message。
- Prompt input: `InputTextMultiline` と `Send` / `Clear` ボタン。input field が active のときは `Ctrl+Enter` でも送信できます。
- Action History: AI exchange、raw response、extracted JSON、action result、token count、生成時間、finish reason。

ユーザーが `Send` を押すと、`SendChatPrompt()` は入力を trim し、`BuildLlmMessages()` で message list を作り、`LocalLlmService::Submit()` に渡します。その後 user message を transcript に追加し、最終的な response とひもづけるために prompt を `m_pendingUserPrompt` に保存します。

`Clear` はセッション内 UI state だけを消します。chat transcript、action history、pending prompt、input buffer、直近の AI diagnostics を消しますが、model unload や scene 変更は行いません。

## LLM Service の流れ

`LocalLlmService` は小さな async API として実装されています。

- `LoadAsync(config)` は既存 model を止め、GGUF path を検証し、`LoadWorker` を開始します。
- `Submit(messages)` は service が `Ready` のときだけ `GenerateWorker` を開始します。
- `CancelGeneration()` は model を保持したまま現在の generation loop に停止要求を出します。
- `Stop()` は worker を join し、model/context を解放し、`Stopped` にします。
- `DrainEvents()` は queued status/response/error events を返して queue を空にします。

`LoadWorker()` は次を作成します。

- 設定された GGUF path から `llama_model`。
- context size と CPU thread count を反映した `llama_context`。
- sampler / grammar 設定が初期化できるか確認するための短命な sampler probe。

`Generate()` は新しい `llama_sampler` chain を作成し、context memory を clear し、model chat template を適用し、prompt を tokenize し、prompt token を `llama_n_batch(context)` 以下の chunk に分けて decode したあと、`maxTokens` まで sampling します。chunked prompt decode により、大きな scene でも llama.cpp に大きすぎる prompt batch を渡さないようにしています。response event には次の情報が入ります。

- raw generated text
- prompt token count
- output token count
- elapsed milliseconds
- finish reason
- grammar が有効だったか
- sampler diagnostics

## Structured JSON Generation

`LocalLlmConfig::structuredJson` が true の場合、`CreateSamplerChain()` は `llama_sampler_init_grammar()` で GBNF grammar sampler を追加しようとします。

grammar は top-level response を次の形に制限します。

```json
{
  "reply": "...",
  "actions": [
    { "method": "set_view_settings", "params": {} }
  ]
}
```

許可している method は次のとおりです。

- `set_view_settings`
- `set_environment_settings`
- `set_sun_settings`
- `set_shadow_settings`
- `set_material_preview`
- `set_camera`
- `set_model_transform`

grammar の初期化に失敗した場合、service は unconstrained generation に fallback し、diagnostics を出します。安全性は grammar だけではなく、最終的に C++ 側の validation path で担保します。

## Response Handling と Actions

`DrainLlmEvents()` は response event を `HandleLlmResponse()` に渡します。

`HandleLlmResponse()` は次の順で処理します。

1. raw response と runtime stats を `AiExchange` に保存する。
2. generated text から最初の完全な JSON object を抽出する。
3. `JsonParser` で JSON を parse する。
4. top-level object と string の `reply` を必須にする。
5. `actions` がない場合は action なしとして扱う。
6. 各 action が object であり、supported `method` と object `params` を持つことを確認する。
7. `ApplyAiAction(..., commit=false)` で全 action を検証する。
8. すべて通った場合のみ、`ApplyAiAction(..., commit=true)` で反映する。
9. assistant reply、diagnostics、action log を UI に追加する。

この 2 pass flow により、部分的な state 変更を防ぎます。どれか 1 つでも malformed action や unsupported action がある場合、response 全体を reject し、scene/material/camera/view state は変更しません。

## Threading Rules

この接続では ownership を明確に分けています。

- Worker thread: llama.cpp model/context、generation ごとの sampler chain、event production。
- UI thread: ImGui、project state、renderer state、chat transcript、action application。

worker thread は `LocalLlmEvent` だけで UI に通知します。`m_stopRequested` は atomic、event/status/model pointer は `m_mutex` で保護します。

これにより、ImGui と D3D12 呼び出しが worker thread に流れず、renderer state の cross-thread mutation も避けられます。

## Persistence

Project JSON には LLM runtime settings を保存します。具体的には model path、context tokens、max tokens、GPU layers、CPU threads、sampling settings、`structuredJson` などです。

Action History はセッション内診断用です。project file には保存しません。

## Prompt State Size

`BuildControlStateJson()` は renderer state を compact にして LLM に渡します。大きな scene では material 数が多くなるため、すべての material を serializing するのではなく、`materialCount`、選択中 material 名、上限付きの `materialsPreview` を prompt に含めます。選択中 material は必ず preview に含めます。

## Notes

- ChatLookDev は `llama-server` ではなく、in-process の llama.cpp を使います。
- この接続には MCP bridge、runtime shader compiler、shader editing path は含まれません。
- grammar は generation constraint として使います。action 適用の最終判断は C++ validation が行います。
