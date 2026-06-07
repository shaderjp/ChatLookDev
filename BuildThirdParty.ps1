param(
    [Parameter(Mandatory = $true)][string]$AssimpRoot,
    [Parameter(Mandatory = $true)][string]$AssimpBuildRoot,
    [Parameter(Mandatory = $true)][string]$AssimpLibDir,
    [Parameter(Mandatory = $true)][string]$AssimpZlibDir,
    [Parameter(Mandatory = $true)][string]$AssimpLibName,
    [Parameter(Mandatory = $true)][string]$AssimpZlibName,
    [Parameter(Mandatory = $true)][string]$DirectXTexProject,
    [Parameter(Mandatory = $true)][string]$DirectXTexLibDir,
    [Parameter(Mandatory = $true)][string]$LlamaCppRoot,
    [Parameter(Mandatory = $true)][string]$LlamaCppBuildRoot,
    [Parameter(Mandatory = $true)][string]$MSBuildPath,
    [Parameter(Mandatory = $true)][string]$Configuration,
    [ValidateSet('ON', 'OFF')][string]$LlamaCuda = 'OFF',
    [ValidateSet('ON', 'OFF')][string]$LlamaVulkan = 'OFF'
)

$ErrorActionPreference = 'Stop'

function Invoke-WithMutex {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $mutex = [System.Threading.Mutex]::new($false, $Name)
    try {
        [void]$mutex.WaitOne()
        & $Body
    }
    finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

function Test-CudaToolkit {
    if ($env:CUDA_PATH) {
        $nvccPath = Join-Path $env:CUDA_PATH 'bin\nvcc.exe'
        if (Test-Path $nvccPath) {
            return $true
        }
    }
    return [bool](Get-Command nvcc -ErrorAction SilentlyContinue)
}

function Test-VulkanSdk {
    if ($env:VULKAN_SDK) {
        $glslcPath = Join-Path $env:VULKAN_SDK 'Bin\glslc.exe'
        $libraryPath = Join-Path $env:VULKAN_SDK 'Lib\vulkan-1.lib'
        $headerPath = Join-Path $env:VULKAN_SDK 'Include\vulkan\vulkan.h'
        if ((Test-Path $glslcPath) -and (Test-Path $libraryPath) -and (Test-Path $headerPath)) {
            return $true
        }
    }
    return [bool](Get-Command glslc -ErrorAction SilentlyContinue)
}

function Get-CMakeBool {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (!(Test-Path $CachePath)) {
        return $null
    }

    $pattern = "^$([regex]::Escape($Name)):BOOL=(.+)$"
    foreach ($line in Get-Content $CachePath) {
        if ($line -match $pattern) {
            return $Matches[1]
        }
    }
    return $null
}

function Copy-FirstMatch {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string[]]$Patterns,
        [Parameter(Mandatory = $true)][string]$DestinationName
    )

    foreach ($pattern in $Patterns) {
        $candidate = Get-ChildItem -Path $Directory -Filter $pattern -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) {
            $destination = Join-Path $Directory $DestinationName
            Copy-Item -LiteralPath $candidate.FullName -Destination $destination -Force
            return $destination
        }
    }
    throw "No library matching '$($Patterns -join ', ')' was found in $Directory."
}

function Get-CMakeGeneratorArgs {
    $help = cmake --help 2>$null
    if ($help -match 'Visual Studio 18 2026') {
        return @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    }
    return @('-G', 'Visual Studio 17 2022', '-A', 'x64')
}

$cudaToolkitAvailable = Test-CudaToolkit
if ($LlamaCuda -eq 'ON' -and !$cudaToolkitAvailable) {
    throw 'LlamaCuda=ON was requested, but CUDA Toolkit nvcc was not found. Install CUDA Toolkit or build with /p:LlamaCuda=OFF.'
}
$vulkanSdkAvailable = Test-VulkanSdk
if ($LlamaVulkan -eq 'ON' -and !$vulkanSdkAvailable) {
    throw 'LlamaVulkan=ON was requested, but Vulkan SDK glslc, vulkan-1.lib, or vulkan.h was not found. Install Vulkan SDK or build with /p:LlamaVulkan=OFF.'
}
$llamaCudaCmake = if ($LlamaCuda -eq 'ON') { 'ON' } else { 'OFF' }
$llamaVulkanCmake = if ($LlamaVulkan -eq 'ON') { 'ON' } else { 'OFF' }
$cmakeGeneratorArgs = Get-CMakeGeneratorArgs

Invoke-WithMutex -Name "Local\ChatLookDev-DirectXTex-$Configuration" -Body {
    $directXTexLib = Join-Path $DirectXTexLibDir 'DirectXTex.lib'
    if (!(Test-Path $directXTexLib)) {
        & $MSBuildPath $DirectXTexProject /p:Platform=x64 /p:Configuration=$Configuration /v:minimal
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}

Invoke-WithMutex -Name "Local\ChatLookDev-Assimp-$Configuration" -Body {
    $cachePath = Join-Path $AssimpBuildRoot 'CMakeCache.txt'
    $solutionPath = Join-Path $AssimpBuildRoot 'Assimp.sln'
    $assimpNormalized = Join-Path $AssimpLibDir $AssimpLibName
    $zlibNormalized = Join-Path $AssimpZlibDir $AssimpZlibName

    if (!(Test-Path $cachePath) -or !(Test-Path $solutionPath)) {
        cmake -S $AssimpRoot -B $AssimpBuildRoot @cmakeGeneratorArgs `
            -DASSIMP_BUILD_TESTS=OFF `
            -DASSIMP_BUILD_ASSIMP_TOOLS=OFF `
            -DBUILD_SHARED_LIBS=OFF `
            -DASSIMP_INSTALL=OFF `
            -DASSIMP_WARNINGS_AS_ERRORS=OFF
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }

    if (!(Test-Path $assimpNormalized) -or !(Test-Path $zlibNormalized)) {
        cmake --build $AssimpBuildRoot --config $Configuration --target assimp --parallel
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        New-Item -ItemType Directory -Force -Path $AssimpLibDir | Out-Null
        New-Item -ItemType Directory -Force -Path $AssimpZlibDir | Out-Null
        if ($Configuration -eq 'Debug') {
            Copy-FirstMatch -Directory $AssimpLibDir -Patterns @('assimp-vc*-mtd.lib', 'assimp*.lib') -DestinationName $AssimpLibName | Out-Null
            Copy-FirstMatch -Directory $AssimpZlibDir -Patterns @('zlibstaticd.lib', 'zlib*.lib') -DestinationName $AssimpZlibName | Out-Null
        }
        else {
            Copy-FirstMatch -Directory $AssimpLibDir -Patterns @('assimp-vc*-mt.lib', 'assimp*.lib') -DestinationName $AssimpLibName | Out-Null
            Copy-FirstMatch -Directory $AssimpZlibDir -Patterns @('zlibstatic.lib', 'zlib*.lib') -DestinationName $AssimpZlibName | Out-Null
        }
    }
}

Invoke-WithMutex -Name "Local\ChatLookDev-LlamaCpp-$Configuration-$LlamaCuda-$LlamaVulkan" -Body {
    $cachePath = Join-Path $LlamaCppBuildRoot 'CMakeCache.txt'
    $llamaLib = Join-Path (Join-Path (Join-Path $LlamaCppBuildRoot 'src') $Configuration) 'llama.lib'
    $ggmlLib = Join-Path (Join-Path (Join-Path $LlamaCppBuildRoot 'ggml\src') $Configuration) 'ggml.lib'
    $currentCuda = Get-CMakeBool -CachePath $cachePath -Name 'GGML_CUDA'
    $currentVulkan = Get-CMakeBool -CachePath $cachePath -Name 'GGML_VULKAN'
    $configureLlama = !(Test-Path $cachePath) -or ($currentCuda -ne $llamaCudaCmake) -or ($currentVulkan -ne $llamaVulkanCmake)

    if ($configureLlama) {
        Write-Host "Configuring llama.cpp GGML_CUDA=$llamaCudaCmake GGML_VULKAN=$llamaVulkanCmake (LlamaCuda=$LlamaCuda, CUDA toolkit detected=$cudaToolkitAvailable, LlamaVulkan=$LlamaVulkan, Vulkan SDK detected=$vulkanSdkAvailable)"
        $llamaConfigureArgs = @(
            '-S', $LlamaCppRoot,
            '-B', $LlamaCppBuildRoot
        ) + $cmakeGeneratorArgs + @(
            '-DBUILD_SHARED_LIBS=OFF',
            '-DLLAMA_BUILD_COMMON=OFF',
            '-DLLAMA_BUILD_TESTS=OFF',
            '-DLLAMA_BUILD_TOOLS=OFF',
            '-DLLAMA_BUILD_EXAMPLES=OFF',
            '-DLLAMA_BUILD_SERVER=OFF',
            '-DLLAMA_BUILD_APP=OFF',
            "-DGGML_CUDA=$llamaCudaCmake",
            "-DGGML_VULKAN=$llamaVulkanCmake"
        )
        cmake @llamaConfigureArgs
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }

    if (!(Test-Path $llamaLib) -or !(Test-Path $ggmlLib) -or $configureLlama) {
        cmake --build $LlamaCppBuildRoot --config $Configuration --target llama --parallel
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}
