# ChatLookDev

English version: [README.md](README.md)

ChatLookDev は、glTF/GLB の PBR モデルを確認するための小さな Direct3D 12 LookDev アプリケーションです。HDR IBL、ImGui docking ベースの操作 UI、llama.cpp を使った in-process のチャットパネルを備えています。

このリポジトリは Codex とバイブコーディングで作成しているサンプルです。完成済みのプロダクションツールではなく、ローカル AI と対話しながら LookDev 環境を育てていくための小さな参照実装として扱っています。

![ChatLookDev screenshot](images/screenshot.png)

## スコープ

- Direct3D 12 の raster PBR パイプライン。
- NuGet から取得する Agility SDK と DXC。
- `Bin/Shaders` へのビルド時 HLSL コンパイル。
- Assimp による glTF/GLB インポート。
- base color、normal、roughness、metallic、occlusion、emissive、alpha mode、tangent data。
- DirectXTex による `.hdr` と float DDS の環境テクスチャ読み込み。
- GPU 上での split-sum IBL 事前計算。
- Sun + IBL の物理ベース風コントロール: 太陽光の照度 lux、露出 EV、HDRI intensity multiplier。
- ImGui docking パネル: Viewport、Scene、Material、Lighting、AI Chat、Diagnostics / Stats。
- ImGui と AI action system からのモデル transform 制御。
- llama.cpp の in-process ローカル LLM サービス。既定は CPU-only。任意で CUDA または Vulkan backend ビルドに対応。

Shader editing、MCP、automation bridge、runtime shader compilation、DXR/path tracing、Vulkan renderer、mesh shader 実験は v1 では意図的に対象外です。

## 要件

- Visual Studio 2026 Insiders。MSBuild は `C:\Program Files\Microsoft Visual Studio\18\Insiders` 配下を想定。
- Platform toolset `v145`。
- Windows SDK `10.0.26100.0`。
- Assimp と llama.cpp の依存ビルド用 CMake。
- 任意: `/p:LlamaCuda=ON` でビルドする場合のみ CUDA Toolkit。
- 任意: llama.cpp Vulkan backend を `/p:LlamaVulkan=ON` でビルドする場合のみ Vulkan SDK。

## ビルド

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Debug /p:Platform=x64 /p:LlamaCuda=OFF
```

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

## モデルの配置

既定の GGUF パスは次のとおりです。

```text
Assets/Models/gemma-4-E4B-it/gemma-4-E4B-it-Q4_K_M.gguf
```

GGUF ファイルは git の管理対象外です。モデルが配置されていない場合でもアプリケーションは起動し、AI Chat パネルにファイルが見つからないことを表示します。

## プロジェクトファイル

プロジェクトは `.chatlookdev.json` として保存され、次の情報を含みます。

- scene path
- HDRI path
- model transform
- camera
- Sun / IBL / view settings
- material overrides
- local LLM runtime settings

Shader source は保存されません。
