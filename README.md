# ChatLookDev

ChatLookDev is a small Direct3D 12 LookDev application for glTF/GLB PBR inspection with HDR IBL, ImGui docking controls, and an in-process llama.cpp chat panel.

## Scope

- Direct3D 12 raster PBR pipeline.
- Agility SDK and DXC from NuGet.
- Build-time HLSL compilation to `Bin/Shaders`.
- glTF/GLB import through Assimp.
- Base color, normal, roughness, metallic, occlusion, emissive, alpha mode, and tangent data.
- `.hdr` and float DDS environment textures through DirectXTex.
- Split-sum IBL precomputation on GPU.
- Sun + IBL physical-style controls: sun illuminance in lux, exposure EV, HDRI intensity multiplier.
- ImGui docking panels: Viewport, Scene, Material, Lighting, AI Chat, Diagnostics / Stats.
- llama.cpp in-process local LLM service, CPU-only by default.

Shader editing, MCP, automation bridges, runtime shader compilation, DXR/path tracing, Vulkan, and mesh shader experiments are intentionally out of v1.

## Requirements

- Visual Studio 2026 Insiders with MSBuild under `C:\Program Files\Microsoft Visual Studio\18\Insiders`.
- Platform toolset `v145`.
- Windows SDK `10.0.26100.0`.
- CMake for Assimp and llama.cpp dependency builds.
- Optional: CUDA Toolkit only when building with `/p:LlamaCuda=ON`.

## Build

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Debug /p:Platform=x64 /p:LlamaCuda=OFF
```

The default LLM build is CPU-only. To request CUDA explicitly:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe" .\ChatLookDev.sln /m /p:Configuration=Release /p:Platform=x64 /p:LlamaCuda=ON
```

`LlamaCuda=ON` fails early if `nvcc` is not found.

## Model Path

The default GGUF path is:

```text
Assets/Models/gemma-4-E4B-it/gemma-4-E4B-it-Q4_K_M.gguf
```

GGUF files are ignored by git. The application still starts without the model and reports the missing file in the AI Chat panel.

## Project Files

Projects are saved as `.chatlookdev.json` and include:

- scene path
- HDRI path
- camera
- Sun / IBL / view settings
- material overrides
- local LLM runtime settings

Shader source is not serialized.
