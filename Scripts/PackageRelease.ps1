param(
    [string]$Version = 'v1.1.1',
    [ValidateSet('CPU', 'Vulkan')][string]$Backend = 'CPU',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$CreateTag,
    [switch]$PublishGitHubRelease
)

$ErrorActionPreference = 'Stop'

function Resolve-MSBuildPath {
    $candidates = @()
    if ($env:MSBUILD_EXE) {
        $candidates += $env:MSBUILD_EXE
    }
    $candidates += 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe'
    $command = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($command) {
        $candidates += $command.Source
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw 'MSBuild was not found. Install Visual Studio 2026 Insiders or set MSBUILD_EXE.'
}

function Copy-DirectoryFresh {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (!(Test-Path $Source)) {
        throw "Required directory was not found: $Source"
    }
    if (Test-Path $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

function Copy-FileRequired {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (!(Test-Path $Source)) {
        throw "Required file was not found: $Source"
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Get-GitHubRepoSlug {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $origin = (& git -C $RepoRoot remote get-url origin 2>$null).Trim()
    if ($origin -match 'github\.com[:/](?<owner>[^/]+)/(?<repo>.+?)(?:\.git)?$') {
        return "$($Matches.owner)/$($Matches.repo)"
    }
    throw "Could not derive a GitHub repository from origin: $origin"
}

function Get-GitHubToken {
    if (![string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
        return $env:GITHUB_TOKEN
    }

    $credentialInput = "protocol=https`nhost=github.com`n`n"
    $credentials = $credentialInput | git credential fill 2>$null
    $passwordLine = $credentials | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
    if ($passwordLine) {
        return $passwordLine.Substring('password='.Length)
    }

    return $null
}

function Invoke-GitHubJson {
    param(
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][hashtable]$Headers,
        [object]$Body = $null
    )

    if ($null -eq $Body) {
        return Invoke-RestMethod -Method $Method -Uri $Uri -Headers $Headers
    }

    $json = $Body | ConvertTo-Json -Depth 10
    return Invoke-RestMethod -Method $Method -Uri $Uri -Headers $Headers -ContentType 'application/json; charset=utf-8' -Body $json
}

function Publish-Release {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Body,
        [Parameter(Mandatory = $true)][string[]]$AssetPaths
    )

    $repoSlug = Get-GitHubRepoSlug -RepoRoot $RepoRoot
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if ($gh) {
        & gh release view $Tag --repo $repoSlug *> $null
        if ($LASTEXITCODE -ne 0) {
            & gh release create $Tag --repo $repoSlug --title $Title --notes $Body @AssetPaths
        }
        else {
            & gh release edit $Tag --repo $repoSlug --title $Title --notes $Body
            & gh release upload $Tag --repo $repoSlug @AssetPaths --clobber
        }
        if ($LASTEXITCODE -ne 0) {
            throw 'GitHub release publish failed through gh.'
        }
        return
    }

    $token = Get-GitHubToken
    if ([string]::IsNullOrWhiteSpace($token)) {
        throw 'GitHub release publish requested, but gh and GitHub API credentials were not available.'
    }

    $headers = @{
        Authorization = "Bearer $token"
        Accept = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
        'User-Agent' = 'ChatLookDev-PackageRelease'
    }

    $api = "https://api.github.com/repos/$repoSlug"
    $release = $null
    try {
        $release = Invoke-GitHubJson -Method Get -Uri "$api/releases/tags/$Tag" -Headers $headers
    }
    catch {
        $statusCode = $null
        if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
            $statusCode = [int]$_.Exception.Response.StatusCode
        }
        if ($statusCode -ne 404) {
            throw
        }
    }

    if ($null -eq $release) {
        $release = Invoke-GitHubJson -Method Post -Uri "$api/releases" -Headers $headers -Body @{
            tag_name = $Tag
            target_commitish = 'main'
            name = $Title
            body = $Body
            draft = $false
            prerelease = $false
        }
    }
    else {
        $release = Invoke-GitHubJson -Method Patch -Uri "$api/releases/$($release.id)" -Headers $headers -Body @{
            name = $Title
            body = $Body
            draft = $false
            prerelease = $false
        }
    }

    $assetNames = $AssetPaths | ForEach-Object { [IO.Path]::GetFileName($_) }
    $existingAssets = Invoke-GitHubJson -Method Get -Uri "$api/releases/$($release.id)/assets?per_page=100" -Headers $headers
    foreach ($asset in @($existingAssets)) {
        if ($assetNames -contains $asset.name) {
            Invoke-RestMethod -Method Delete -Uri "$api/releases/assets/$($asset.id)" -Headers $headers | Out-Null
        }
    }

    $uploadBase = ([string]$release.upload_url).Split('{')[0]
    foreach ($assetPath in $AssetPaths) {
        $assetName = [IO.Path]::GetFileName($assetPath)
        $uploadUri = "${uploadBase}?name=$([Uri]::EscapeDataString($assetName))"
        Invoke-RestMethod -Method Post -Uri $uploadUri -Headers $headers -ContentType 'application/octet-stream' -InFile $assetPath | Out-Null
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tag = if ($Version.StartsWith('v')) { $Version } else { "v$Version" }
$backendLower = $Backend.ToLowerInvariant()
$llamaVulkan = if ($Backend -eq 'Vulkan') { 'ON' } else { 'OFF' }
$packageBaseName = "ChatLookDev-$tag-win64-$backendLower"
$solutionPath = Join-Path $repoRoot 'ChatLookDev.sln'
$outDir = Join-Path $repoRoot "Bin\x64\$Configuration"
$shaderDir = Join-Path $repoRoot 'Bin\Shaders'
$distDir = Join-Path $repoRoot 'dist'
$packageDir = Join-Path $distDir $packageBaseName
$symbolsDir = Join-Path $distDir "$packageBaseName-symbols"
$runtimeZip = Join-Path $distDir "$packageBaseName.zip"
$symbolsZip = Join-Path $distDir "$packageBaseName-symbols.zip"
$shaFile = Join-Path $distDir 'SHA256SUMS.txt'
$commitText = (& git -C $repoRoot rev-parse --short HEAD).Trim()
if ((& git -C $repoRoot status --porcelain).Count -gt 0) {
    $commitText = "$commitText (dirty working tree)"
}

New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$msbuild = Resolve-MSBuildPath
Write-Host "Building ChatLookDev $Configuration x64 Backend=$Backend (LlamaVulkan=$llamaVulkan)"
& $msbuild $solutionPath /m /p:Configuration=$Configuration /p:Platform=x64 /p:LlamaCuda=OFF /p:LlamaVulkan=$llamaVulkan /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE."
}

if (Test-Path $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
if (Test-Path $symbolsDir) {
    Remove-Item -LiteralPath $symbolsDir -Recurse -Force
}
Remove-Item -LiteralPath $runtimeZip, $symbolsZip -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $packageDir, $symbolsDir | Out-Null

Copy-FileRequired -Source (Join-Path $outDir 'ChatLookDev.exe') -Destination (Join-Path $packageDir 'ChatLookDev.exe')
Copy-DirectoryFresh -Source (Join-Path $outDir 'D3D12') -Destination (Join-Path $packageDir 'D3D12')
Copy-DirectoryFresh -Source $shaderDir -Destination (Join-Path $packageDir 'Bin\Shaders')
Copy-FileRequired -Source (Join-Path $repoRoot 'README.md') -Destination (Join-Path $packageDir 'README.md')
Copy-FileRequired -Source (Join-Path $repoRoot 'README.ja.md') -Destination (Join-Path $packageDir 'README.ja.md')
Copy-FileRequired -Source (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $packageDir 'LICENSE')
Copy-FileRequired -Source (Join-Path $repoRoot 'imgui.ini') -Destination (Join-Path $packageDir 'imgui.ini')
Copy-DirectoryFresh -Source (Join-Path $repoRoot 'docs') -Destination (Join-Path $packageDir 'docs')
Copy-DirectoryFresh -Source (Join-Path $repoRoot 'images') -Destination (Join-Path $packageDir 'images')

$modelDir = Join-Path $packageDir 'Assets\Models\gemma-4-E4B-it'
New-Item -ItemType Directory -Force -Path $modelDir | Out-Null
@'
Place the GGUF model here if you want to use the default AI Chat path:

gemma-4-E4B-it-Q4_K_M.gguf

GGUF files are intentionally not included in release packages.
You can also browse to another .gguf file from the AI Chat panel.
'@ | Set-Content -LiteralPath (Join-Path $modelDir 'README.txt') -Encoding UTF8

Get-ChildItem -LiteralPath $packageDir -Recurse -File -Filter '*.gguf' -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -LiteralPath $packageDir -Recurse -File -Filter '*.pdb' -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item -LiteralPath (Join-Path $packageDir 'imgui.user.ini') -Force -ErrorAction SilentlyContinue

$backendNotes = if ($Backend -eq 'Vulkan') {
    @'
This package was built with the llama.cpp Vulkan backend. For GPU offload:
- Install a GPU driver with Vulkan runtime support.
- Keep GPU Layers greater than 0 in the AI Chat panel.
- The AI Chat and Diagnostics panels show whether inference is CPU or GPU offload.
'@
}
else {
    @'
This package was built for CPU inference. It is intended to start on machines without CUDA or Vulkan SDK installations.
GPU offload is not expected in this package.
'@
}

$backendNotesJa = if ($Backend -eq 'Vulkan') {
    @'
このパッケージは llama.cpp Vulkan backend 付きでビルドされています。GPU offload を使う場合:
- Vulkan runtime 対応の GPU driver をインストールしてください。
- AI Chat panel の GPU Layers を 0 より大きい値にしてください。
- AI Chat と Diagnostics panel で CPU/GPU どちらの推論 mode か確認できます。
'@
}
else {
    @'
このパッケージは CPU 推論向けにビルドされています。CUDA や Vulkan SDK が入っていない環境でも起動できることを意図しています。
GPU offload はこのパッケージでは想定していません。
'@
}

@"
ChatLookDev $tag Windows x64 $Backend package

Run ChatLookDev.exe from this folder.

$backendNotes
The default ImGui docking layout is seeded from imgui.ini. Runtime changes are saved to imgui.user.ini.

The GGUF model is not included. Place it at:
Assets/Models/gemma-4-E4B-it/gemma-4-E4B-it-Q4_K_M.gguf

You can also browse to another .gguf from the AI Chat panel.

Built from commit $commitText.
"@ | Set-Content -LiteralPath (Join-Path $packageDir 'README-RELEASE.txt') -Encoding UTF8

@"
ChatLookDev $tag Windows x64 $Backend package

このフォルダから ChatLookDev.exe を実行してください。

$backendNotesJa
既定の ImGui docking layout は imgui.ini から読み込まれます。実行時の layout 変更は imgui.user.ini に保存されます。

GGUF model は含まれていません。既定 path を使う場合は次の場所へ配置してください:
Assets/Models/gemma-4-E4B-it/gemma-4-E4B-it-Q4_K_M.gguf

AI Chat panel から別の .gguf を参照することもできます。

ビルド元 commit: $commitText.
"@ | Set-Content -LiteralPath (Join-Path $packageDir 'README-RELEASE.ja.txt') -Encoding UTF8

Copy-FileRequired -Source (Join-Path $outDir 'ChatLookDev.pdb') -Destination (Join-Path $symbolsDir 'ChatLookDev.pdb')
@"
ChatLookDev $tag Windows x64 $Backend symbols

This package contains PDB symbols for:
$packageBaseName.zip
"@ | Set-Content -LiteralPath (Join-Path $symbolsDir 'README-SYMBOLS.txt') -Encoding UTF8

Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $runtimeZip -CompressionLevel Optimal
Compress-Archive -Path (Join-Path $symbolsDir '*') -DestinationPath $symbolsZip -CompressionLevel Optimal

$zipFiles = Get-ChildItem -LiteralPath $distDir -File -Filter "ChatLookDev-$tag-win64-*.zip" | Sort-Object Name
$hashLines = foreach ($zip in $zipFiles) {
    $hash = (Get-FileHash -LiteralPath $zip.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($zip.Name)"
}
$hashLines | Set-Content -LiteralPath $shaFile -Encoding ASCII

if ($CreateTag) {
    $existingTag = (& git -C $repoRoot tag --list $tag).Trim()
    if ([string]::IsNullOrWhiteSpace($existingTag)) {
        & git -C $repoRoot tag -a $tag -m "ChatLookDev $tag"
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to create tag $tag."
        }
    }
    & git -C $repoRoot push origin $tag
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to push tag $tag."
    }
}

if ($PublishGitHubRelease) {
    $assetPaths = @($zipFiles | ForEach-Object { $_.FullName })
    $assetPaths += $shaFile
    $shaText = Get-Content -LiteralPath $shaFile -Raw
    $releaseBody = @"
# ChatLookDev $tag

Windows x64 release packages for CPU and optional Vulkan LLM inference.

## Notes
- GGUF model files are not included.
- CPU packages start without CUDA or Vulkan SDK installations.
- Vulkan packages require a Vulkan-capable GPU driver/runtime for GPU offload.
- The default UI layout is seeded from imgui.ini; runtime changes are saved to imgui.user.ini.

## SHA256
````text
$shaText
````
"@
    Publish-Release -RepoRoot $repoRoot -Tag $tag -Title "ChatLookDev $tag" -Body $releaseBody -AssetPaths $assetPaths
}

Write-Host "Created package: $runtimeZip"
Write-Host "Created symbols: $symbolsZip"
Write-Host "Updated hashes: $shaFile"
