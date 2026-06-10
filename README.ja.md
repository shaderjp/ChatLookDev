# ChatLookDev

English version: [README.md](README.md)

ChatLookDev は、glTF/GLB の PBR モデルを確認するための小さな Direct3D 12 LookDev アプリケーションです。HDR IBL、ImGui docking ベースの操作 UI、llama.cpp を使った in-process のチャットパネルを備えています。

このリポジトリは Codex とバイブコーディングで作成しているサンプルです。完成済みのプロダクションツールではなく、ローカル AI と対話しながら LookDev 環境を育てていくための小さな参照実装として扱っています。

![ChatLookDev screenshot](images/screenshot.png)

v1.2 の追加表示例:

![Sponza with Sun shadow](images/image2.png)

![Normal display mode](images/image3.png)

## スコープ

- Direct3D 12 の raster PBR パイプライン。
- NuGet から取得する Agility SDK と DXC。
- `Bin/Shaders` へのビルド時 HLSL コンパイル。
- Assimp による glTF/GLB インポート。
- base color、normal、roughness、metallic、occlusion、emissive、alpha mode、tangent data。
- DirectXTex による `.hdr` と float DDS の環境テクスチャ読み込み。
- GPU 上での split-sum IBL 事前計算。
- Sun + IBL の物理ベース風コントロール: 太陽光の照度 lux、露出 EV、HDRI intensity multiplier。
- 単一 Sun shadow map。strength、bias、softness、fit scale、Shadow Mask debug view に対応。
- Normal や Shadow Mask などの debug display mode。
- ImGui docking パネル: Viewport、Scene、Material、Lighting、AI Chat、Diagnostics / Stats。
- Camera overlay、scene framing preset、FOV control、3 つの camera bookmark slot。
- ImGui と AI action system からのモデル transform 制御。
- ImGui と AI action system からの camera preset / bookmark 制御。
- llama.cpp の in-process ローカル LLM サービス。既定は CPU-only。任意で CUDA または Vulkan backend ビルドに対応。

Shader editing、MCP、automation bridge、runtime shader compilation、DXR/path tracing、Vulkan renderer、mesh shader 実験は v1 では意図的に対象外です。

## 要件

- Visual Studio 2026 Insiders + toolset `v145`、または Visual Studio 2022 + toolset `v143`。
- Windows SDK `10.0.26100.0`。
- Assimp と llama.cpp の依存ビルド用 CMake。
- 任意: `/p:LlamaCuda=ON` でビルドする場合のみ CUDA Toolkit。
- 任意: llama.cpp Vulkan backend を `/p:LlamaVulkan=ON` でビルドする場合のみ Vulkan SDK。

## ビルド

Visual Studio 2026:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Debug /p:Platform=x64 /p:LlamaCuda=OFF
```

Visual Studio 2022:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Debug /p:Platform=x64 /p:LlamaCuda=OFF
```

VS 2026 の MSBuild では `v145`、VS 2022 の MSBuild では `v143` を project 側で自動選択します。

既定の LLM ビルドは CPU-only です。CUDA を明示的に要求する場合:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Release /p:Platform=x64 /p:LlamaCuda=ON
```

`LlamaCuda=ON` は `nvcc` が見つからない場合に早期に失敗します。

llama.cpp Vulkan backend を明示的に要求する場合:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Release /p:Platform=x64 /p:LlamaVulkan=ON
```

`LlamaVulkan=ON` は、`VULKAN_SDK` が `Bin/glslc.exe`、`Lib/vulkan-1.lib`、`Include/vulkan/vulkan.h` を含む Vulkan SDK インストールを指している必要があります。

## Release package

CPU package:

```powershell
.\Scripts\PackageRelease.ps1 -Version v1.2.0 -Backend CPU -Configuration Release
```

Vulkan LLM package:

```powershell
.\Scripts\PackageRelease.ps1 -Version v1.2.0 -Backend Vulkan -Configuration Release
```

このスクリプトは選択した backend をビルドし、runtime ZIP、symbols ZIP、`dist/SHA256SUMS.txt` を生成します。runtime package には `ChatLookDev.exe`、Agility SDK DLL、事前コンパイル済み shader、documentation、screenshot、`imgui.ini`、英語/日本語 release notes、model 配置メモを含めます。GGUF model、PDB、build cache、`imgui.user.ini` は runtime ZIP に含めません。

認証情報が使える環境で git tag 作成と GitHub Release の作成/更新まで行う場合:

```powershell
.\Scripts\PackageRelease.ps1 -Version v1.2.0 -Backend Vulkan -Configuration Release -CreateTag -PublishGitHubRelease
```

## モデルの配置

既定の GGUF パスは次のとおりです。

```text
Assets/Models/gemma-4-E4B-it/gemma-4-E4B-it-Q4_K_M.gguf
```

GGUF ファイルは git の管理対象外です。モデルが配置されていない場合でもアプリケーションは起動し、AI Chat パネルにファイルが見つからないことを表示します。

## ドキュメント

- [LLM と ImGui の接続](docs/llm-imgui-integration.ja.md)
- [LookDev rendering notes](docs/lookdev-rendering.ja.md)

## UI layout

Git 管理される `imgui.ini` は既定 docking layout の seed です。実行時の layout 変更は git 管理外の `imgui.user.ini` に保存されます。`Project > Reset UI Layout` を使うと、ユーザー layout を破棄して既定 layout を再読み込みできます。

読みやすさの per-user 設定は git 管理外の `ui.user.json` に保存します。`Project > UI Settings` から UI font size、chat font size、UI scale、chat transcript height を調整できます。font、scale、padding の変更は次回起動時に反映され、chat transcript height は即時反映されます。`Project > Reset UI Settings` で user settings file を削除し、次回起動時に既定値へ戻せます。

## プロジェクトファイル

プロジェクトは `.chatlookdev.json` として保存され、次の情報を含みます。

- scene path
- HDRI path
- model transform
- camera と camera bookmarks
- Sun / IBL / view settings
- material overrides
- local LLM runtime settings

Shader source は保存されません。
