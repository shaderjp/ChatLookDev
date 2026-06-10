# ChatLookDev

日本語版: [README.ja.md](README.ja.md)

ChatLookDev is a small Direct3D 12 LookDev application for glTF/GLB PBR inspection with HDR IBL, ImGui docking controls, and an in-process llama.cpp chat panel.

This repository is a sample being built with Codex and vibe coding. It is meant as a compact reference for iterating on a local, AI-assisted LookDev workflow rather than a finished production tool.

![ChatLookDev screenshot](images/screenshot.png)

Additional v1.2 views:

![Sponza with Sun shadow](images/image2.png)

![Normal display mode](images/image3.png)

## Scope

- Direct3D 12 raster PBR pipeline.
- Agility SDK and DXC from NuGet.
- Build-time HLSL compilation to `Bin/Shaders`.
- glTF/GLB import through Assimp.
- Base color, normal, roughness, metallic, occlusion, emissive, alpha mode, and tangent data.
- `.hdr` and float DDS environment textures through DirectXTex.
- Split-sum IBL precomputation on GPU.
- Sun + IBL physical-style controls: sun illuminance in lux, exposure EV, HDRI intensity multiplier.
- Single Sun shadow map with strength, bias, softness, fit scale, and Shadow Mask debug view.
- Debug display modes including Normal and Shadow Mask.
- ImGui docking panels: Viewport, Scene, Material, Lighting, AI Chat, Diagnostics / Stats.
- Camera overlay, scene-framing presets, FOV control, and three camera bookmark slots.
- Model transform controls through ImGui and the AI action system.
- Camera presets and bookmarks through ImGui and the AI action system.
- llama.cpp in-process local LLM service, CPU-only by default with optional CUDA or Vulkan backend builds.

Shader editing, MCP, automation bridges, runtime shader compilation, DXR/path tracing, Vulkan rendering, and mesh shader experiments are intentionally out of v1.

## Requirements

- Visual Studio 2026 Insiders with toolset `v145`, or Visual Studio 2022 with toolset `v143`.
- Windows SDK `10.0.26100.0`.
- CMake for Assimp and llama.cpp dependency builds.
- Optional: CUDA Toolkit only when building with `/p:LlamaCuda=ON`.
- Optional: Vulkan SDK only when building the llama.cpp Vulkan backend with `/p:LlamaVulkan=ON`.

## Build

Visual Studio 2026:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Debug /p:Platform=x64 /p:LlamaCuda=OFF
```

Visual Studio 2022:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Debug /p:Platform=x64 /p:LlamaCuda=OFF
```

The project selects `v145` when built by VS 2026 MSBuild and `v143` when built by VS 2022 MSBuild.

The default LLM build is CPU-only. To request CUDA explicitly:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Release /p:Platform=x64 /p:LlamaCuda=ON
```

`LlamaCuda=ON` fails early if `nvcc` is not found.

To request the llama.cpp Vulkan backend explicitly:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Release /p:Platform=x64 /p:LlamaVulkan=ON
```

`LlamaVulkan=ON` requires `VULKAN_SDK` to point at a Vulkan SDK installation containing `Bin/glslc.exe`, `Lib/vulkan-1.lib`, and `Include/vulkan/vulkan.h`.

## Release Packages

CPU package:

```powershell
.\Scripts\PackageRelease.ps1 -Version v1.2.0 -Backend CPU -Configuration Release
```

Vulkan LLM package:

```powershell
.\Scripts\PackageRelease.ps1 -Version v1.2.0 -Backend Vulkan -Configuration Release
```

The script builds the selected backend, stages a runtime ZIP, stages a symbols ZIP, and updates `dist/SHA256SUMS.txt`. Runtime packages include `ChatLookDev.exe`, Agility SDK DLLs, precompiled shaders, documentation, screenshots, `imgui.ini`, English/Japanese release notes, and a model placement note. GGUF model files, PDB files, build caches, and `imgui.user.ini` are excluded from runtime ZIPs.

To create the git tag and publish/update a GitHub Release when credentials are available:

```powershell
.\Scripts\PackageRelease.ps1 -Version v1.2.0 -Backend Vulkan -Configuration Release -CreateTag -PublishGitHubRelease
```

## Model Path

The default GGUF path is:

```text
Assets/Models/gemma-4-E4B-it/gemma-4-E4B-it-Q4_K_M.gguf
```

GGUF files are ignored by git. The application still starts without the model and reports the missing file in the AI Chat panel.

## Documentation

- [LLM and ImGui integration](docs/llm-imgui-integration.md)
- [LookDev rendering notes](docs/lookdev-rendering.md)

## UI Layout

The tracked `imgui.ini` file is the default docking layout seed. Runtime layout changes are saved to `imgui.user.ini`, which is ignored by git. Use `Project > Reset UI Layout` to discard the user layout and reload the default layout.

Per-user readability settings are saved to ignored `ui.user.json`. Use `Project > UI Settings` to adjust UI font size, chat font size, UI scale, and chat transcript height. Font, scale, and padding changes are applied on the next launch; chat transcript height applies immediately. Use `Project > Reset UI Settings` to remove the user settings file and return to defaults on next launch.

## Project Files

Projects are saved as `.chatlookdev.json` and include:

- scene path
- HDRI path
- model transform
- camera and camera bookmarks
- Sun / IBL / view settings
- material overrides
- local LLM runtime settings

Shader source is not serialized.
