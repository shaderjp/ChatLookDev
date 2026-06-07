#include "D3D12Backend.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <DirectXTex.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <cwctype>

extern "C" __declspec(dllexport) const UINT D3D12SDKVersion = 619;
extern "C" __declspec(dllexport) const char* D3D12SDKPath = ".\\D3D12\\";

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT SceneSrvDescriptorIndex = 0;
constexpr UINT MaterialTextureSlotCount = static_cast<UINT>(rb::TextureSlot::Count);
constexpr UINT MaterialSrvDescriptorStart = 1;
constexpr UINT MaxMaterialCount = 1024;
constexpr UINT EnvironmentSrvDescriptorIndex = MaterialSrvDescriptorStart + MaxMaterialCount * MaterialTextureSlotCount;
constexpr UINT IrradianceSrvDescriptorIndex = EnvironmentSrvDescriptorIndex + 1;
constexpr UINT PrefilterSrvDescriptorIndex = IrradianceSrvDescriptorIndex + 1;
constexpr UINT BrdfLutSrvDescriptorIndex = PrefilterSrvDescriptorIndex + 1;
constexpr UINT IblUavDescriptorIndex = BrdfLutSrvDescriptorIndex + 1;
constexpr UINT ImGuiSrvDescriptorStart = IblUavDescriptorIndex + 16;
constexpr UINT SrvDescriptorCapacity = ImGuiSrvDescriptorStart + 512;
constexpr UINT TextureSlotBaseColor = static_cast<UINT>(rb::TextureSlot::BaseColor);
constexpr UINT TextureSlotNormal = static_cast<UINT>(rb::TextureSlot::Normal);
constexpr UINT TextureSlotRoughness = static_cast<UINT>(rb::TextureSlot::Roughness);
constexpr UINT TextureSlotMetallic = static_cast<UINT>(rb::TextureSlot::Metallic);
constexpr UINT TextureSlotOcclusion = static_cast<UINT>(rb::TextureSlot::Occlusion);
constexpr UINT TextureSlotEmissive = static_cast<UINT>(rb::TextureSlot::Emissive);
constexpr UINT MaterialTextureBaseColorBit = 1u << TextureSlotBaseColor;
constexpr UINT MaterialTextureNormalBit = 1u << TextureSlotNormal;
constexpr UINT MaterialTextureRoughnessBit = 1u << TextureSlotRoughness;
constexpr UINT MaterialTextureMetallicBit = 1u << TextureSlotMetallic;
constexpr UINT MaterialTextureOcclusionBit = 1u << TextureSlotOcclusion;
constexpr UINT MaterialTextureEmissiveBit = 1u << TextureSlotEmissive;
constexpr UINT IblWidth = 256;
constexpr UINT IblHeight = 128;
constexpr UINT IblMipLevels = 6;
constexpr UINT BrdfLutSize = 128;

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = subresource;
    return barrier;
}

D3D12_RENDER_TARGET_BLEND_DESC DefaultBlend()
{
    D3D12_RENDER_TARGET_BLEND_DESC desc = {};
    desc.BlendEnable = FALSE;
    desc.LogicOpEnable = FALSE;
    desc.SrcBlend = D3D12_BLEND_ONE;
    desc.DestBlend = D3D12_BLEND_ZERO;
    desc.BlendOp = D3D12_BLEND_OP_ADD;
    desc.SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

UINT64 AlignTo(UINT64 value, UINT64 alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

std::string WideAdapterName(const DXGI_ADAPTER_DESC1& desc)
{
    char buffer[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buffer, static_cast<int>(sizeof(buffer)), nullptr, nullptr);
    return buffer;
}

std::wstring LowerExtension(const std::filesystem::path& path)
{
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return extension;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.c_str()), text.size());
}

std::string HResultMessage(HRESULT hr)
{
    std::ostringstream message;
    message << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return message.str();
}

ImFont* AddJapaneseImGuiFont(ImGuiIO& io, float sizePixels)
{
    const char* fontCandidates[] =
    {
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothR.ttc",
        "C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
    };

    ImFontConfig fontConfig;
    fontConfig.FontNo = 0;
    const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesJapanese();
    for (const char* fontPath : fontCandidates)
    {
        std::error_code ec;
        if (!std::filesystem::exists(fontPath, ec))
        {
            continue;
        }
        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, sizePixels, &fontConfig, glyphRanges);
        if (font != nullptr)
        {
            return font;
        }
    }

    fontConfig.SizePixels = sizePixels;
    return io.Fonts->AddFontDefault(&fontConfig);
}

bool LoadJapaneseImGuiFonts(ImGuiIO& io)
{
    ImFont* uiFont = AddJapaneseImGuiFont(io, 16.0f);
    ImFont* chatFont = AddJapaneseImGuiFont(io, 20.0f);
    return uiFont != nullptr && chatFont != nullptr;
}

std::wstring SceneTexturePath(const rb::SceneMaterial& material, std::size_t textureSlot)
{
    switch (textureSlot)
    {
    case 0: return material.baseColorTexturePath;
    case 1: return material.normalTexturePath;
    case 2: return material.roughnessTexturePath;
    case 3: return material.metallicTexturePath;
    case 4: return material.occlusionTexturePath;
    case 5: return material.emissiveTexturePath;
    default: return {};
    }
}
}

namespace cld
{
D3D12Backend::~D3D12Backend()
{
    Shutdown();
}

void D3D12Backend::ThrowIfFailed(HRESULT hr, const char* message) const
{
    if (FAILED(hr))
    {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> D3D12Backend::ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open shader blob: " + path.string());
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

void D3D12Backend::Initialize(HWND hwnd, UINT width, UINT height, const std::filesystem::path& rootDirectory)
{
    m_rootDirectory = rootDirectory;
    m_hwnd = hwnd;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    CreateDeviceObjects(hwnd, m_width, m_height);
    CreateRootSignatures();
    CreatePipelineStates();
    ResetPreviewScene();
    CreateConstantBuffer();
    CreateSceneTarget();
    CreateDefaultMaterialResources();
    CreateIblResources();
    InitializeImGui(hwnd);
}

void D3D12Backend::Shutdown()
{
    if (m_device)
    {
        WaitForGpu();
    }

    if (m_imguiInitialized)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiInitialized = false;
    }

    if (m_constantBuffer)
    {
        m_constantBuffer->Unmap(0, nullptr);
        m_constantBufferMapped = nullptr;
    }

    ReleaseRenderTargets();
    m_sceneTarget.Reset();
    m_sceneDepth.Reset();
    m_fallbackTexture.Reset();
    m_environmentTexture.Reset();
    m_irradianceTexture.Reset();
    m_prefilterTexture.Reset();
    m_brdfLutTexture.Reset();
    m_materialTextures.clear();
    m_materials.clear();
    m_draws.clear();
    m_pbrPipelineState.Reset();
    m_skyPipelineState.Reset();
    m_iblPipelineState.Reset();
    m_graphicsRootSignature.Reset();
    m_computeRootSignature.Reset();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_constantBuffer.Reset();
    m_swapChain.Reset();
    m_commandList.Reset();
    for (auto& allocator : m_commandAllocators)
    {
        allocator.Reset();
    }
    m_commandQueue.Reset();
    m_rtvHeap.Reset();
    m_dsvHeap.Reset();
    m_srvHeap.Reset();
    m_fence.Reset();
    m_device.Reset();
    m_factory.Reset();

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void D3D12Backend::CreateDeviceObjects(HWND hwnd, UINT width, UINT height)
{
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2 failed.");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; m_factory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
        {
            m_adapterName = WideAdapterName(desc);
            break;
        }
    }

    if (!m_device)
    {
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice failed.");
        m_adapterName = "Default D3D12 Adapter";
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_9 };
    m_shaderModel69 = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_9;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "CreateCommandQueue failed.");

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = m_backBufferFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain), "CreateSwapChainForHwnd failed.");
    ThrowIfFailed(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation failed.");
    ThrowIfFailed(swapChain.As(&m_swapChain), "Query IDXGISwapChain3 failed.");
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount + 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "CreateDescriptorHeap(RTV) failed.");
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)), "CreateDescriptorHeap(DSV) failed.");
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = SrvDescriptorCapacity;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)), "CreateDescriptorHeap(SRV) failed.");
    m_srvAllocator.cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_srvAllocator.gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    m_srvAllocator.descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_srvAllocator.used = ImGuiSrvDescriptorStart;
    m_sceneSrvCpu = m_srvAllocator.cpuStart;
    m_sceneSrvGpu = m_srvAllocator.gpuStart;

    for (UINT i = 0; i < FrameCount; ++i)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])), "CreateCommandAllocator failed.");
    }
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_commandList)), "CreateCommandList failed.");
    ThrowIfFailed(m_commandList->Close(), "Initial command list close failed.");

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence failed.");
    m_fenceValues.fill(0);
    m_nextFenceValue = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
    {
        throw std::runtime_error("CreateEvent failed.");
    }

    CreateRenderTargets();
}

void D3D12Backend::ReleaseRenderTargets()
{
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::RtvHandle(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::DsvHandle(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_dsvDescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::SrvCpuHandle(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_srvAllocator.cpuStart;
    handle.ptr += static_cast<SIZE_T>(index) * m_srvAllocator.descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::SrvGpuHandle(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_srvAllocator.gpuStart;
    handle.ptr += static_cast<UINT64>(index) * m_srvAllocator.descriptorSize;
    return handle;
}

void D3D12Backend::CreateRenderTargets()
{
    for (UINT i = 0; i < FrameCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])), "GetBuffer failed.");
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, RtvHandle(i));
    }
}

void D3D12Backend::CreateSceneTarget()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_sceneWidth;
    desc.Height = m_sceneHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = m_sceneFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = m_sceneFormat;
    clearValue.Color[0] = m_lookDevConstants.skyHorizonColor.x;
    clearValue.Color[1] = m_lookDevConstants.skyHorizonColor.y;
    clearValue.Color[2] = m_lookDevConstants.skyHorizonColor.z;
    clearValue.Color[3] = 1.0f;

    const D3D12_HEAP_PROPERTIES heapProps = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&m_sceneTarget)), "Create scene render target failed.");
    m_sceneTargetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_device->CreateRenderTargetView(m_sceneTarget.Get(), nullptr, RtvHandle(FrameCount));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = m_sceneFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_sceneTarget.Get(), &srvDesc, m_sceneSrvCpu);

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_sceneWidth;
    depthDesc.Height = m_sceneHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = m_sceneDepthFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = m_sceneDepthFormat;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&m_sceneDepth)), "Create scene depth target failed.");
    m_device->CreateDepthStencilView(m_sceneDepth.Get(), nullptr, DsvHandle(0));
}

void D3D12Backend::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE materialRange = {};
    materialRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialRange.NumDescriptors = 6;
    materialRange.BaseShaderRegister = 0;
    materialRange.RegisterSpace = 0;
    materialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE iblRange = {};
    iblRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    iblRange.NumDescriptors = 4;
    iblRange.BaseShaderRegister = 6;
    iblRange.RegisterSpace = 0;
    iblRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER graphicsParams[5] = {};
    graphicsParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    graphicsParams[0].Descriptor.ShaderRegister = 0;
    graphicsParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParams[1].DescriptorTable.NumDescriptorRanges = 1;
    graphicsParams[1].DescriptorTable.pDescriptorRanges = &materialRange;
    graphicsParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParams[2].Constants.ShaderRegister = 1;
    graphicsParams[2].Constants.Num32BitValues = sizeof(MaterialConstants) / sizeof(std::uint32_t);
    graphicsParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParams[3].Constants.ShaderRegister = 2;
    graphicsParams[3].Constants.Num32BitValues = sizeof(LookDevConstants) / sizeof(std::uint32_t);
    graphicsParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParams[4].DescriptorTable.NumDescriptorRanges = 1;
    graphicsParams[4].DescriptorTable.pDescriptorRanges = &iblRange;
    graphicsParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC graphicsDesc = {};
    graphicsDesc.NumParameters = _countof(graphicsParams);
    graphicsDesc.pParameters = graphicsParams;
    graphicsDesc.NumStaticSamplers = _countof(samplers);
    graphicsDesc.pStaticSamplers = samplers;
    graphicsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&graphicsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "Serialize graphics root signature failed.");
    }
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_graphicsRootSignature)), "Create graphics root signature failed.");

    D3D12_DESCRIPTOR_RANGE computeSrvRange = {};
    computeSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    computeSrvRange.NumDescriptors = 1;
    computeSrvRange.BaseShaderRegister = 0;
    computeSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE computeUavRange = {};
    computeUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeUavRange.NumDescriptors = 1;
    computeUavRange.BaseShaderRegister = 0;
    computeUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParams[3] = {};
    computeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParams[0].Constants.ShaderRegister = 0;
    computeParams[0].Constants.Num32BitValues = sizeof(IblConstants) / sizeof(std::uint32_t);
    computeParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[1].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[1].DescriptorTable.pDescriptorRanges = &computeSrvRange;
    computeParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[2].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[2].DescriptorTable.pDescriptorRanges = &computeUavRange;
    computeParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC computeSampler = {};
    computeSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    computeSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    computeSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    computeSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    computeSampler.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC computeDesc = {};
    computeDesc.NumParameters = _countof(computeParams);
    computeDesc.pParameters = computeParams;
    computeDesc.NumStaticSamplers = 1;
    computeDesc.pStaticSamplers = &computeSampler;
    hr = D3D12SerializeRootSignature(&computeDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "Serialize compute root signature failed.");
    }
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)), "Create compute root signature failed.");
}

void D3D12Backend::CreatePipelineStates()
{
    const std::filesystem::path shaderDir = m_rootDirectory / "Bin" / "Shaders";
    const auto pbrVs = ReadBinaryFile(shaderDir / "LookDevPBR.VSMain.vs_6_9.cso");
    const auto pbrPs = ReadBinaryFile(shaderDir / "LookDevPBR.PSMain.ps_6_9.cso");
    const auto skyVs = ReadBinaryFile(shaderDir / "Sky.VSMain.vs_6_9.cso");
    const auto skyPs = ReadBinaryFile(shaderDir / "Sky.PSMain.ps_6_9.cso");
    const auto iblCs = ReadBinaryFile(shaderDir / "IblPrecompute.CSMain.cs_6_9.cso");

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = m_graphicsRootSignature.Get();
    psoDesc.VS = { pbrVs.data(), pbrVs.size() };
    psoDesc.PS = { pbrPs.data(), pbrPs.size() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0] = DefaultBlend();
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_sceneFormat;
    psoDesc.DSVFormat = m_sceneDepthFormat;
    psoDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pbrPipelineState)), "Create PBR pipeline state failed.");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyDesc = psoDesc;
    skyDesc.InputLayout = {};
    skyDesc.VS = { skyVs.data(), skyVs.size() };
    skyDesc.PS = { skyPs.data(), skyPs.size() };
    skyDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    skyDesc.DepthStencilState.DepthEnable = FALSE;
    skyDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    skyDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&skyDesc, IID_PPV_ARGS(&m_skyPipelineState)), "Create sky pipeline state failed.");

    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc = {};
    computeDesc.pRootSignature = m_computeRootSignature.Get();
    computeDesc.CS = { iblCs.data(), iblCs.size() };
    ThrowIfFailed(m_device->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&m_iblPipelineState)), "Create IBL pipeline state failed.");
}

void D3D12Backend::CreateGeometry(const std::vector<rb::SceneVertex>& vertices, const std::vector<std::uint32_t>& indices, const std::vector<rb::SceneDraw>& draws)
{
    WaitForGpu();
    m_vertexCount = static_cast<UINT>(vertices.size());
    m_indexCount = static_cast<UINT>(indices.size());
    m_draws = draws;

    const UINT64 vertexBufferSize = sizeof(rb::SceneVertex) * vertices.size();
    const UINT64 indexBufferSize = sizeof(std::uint32_t) * indices.size();
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC vertexDesc = BufferDesc(vertexBufferSize);
    D3D12_RESOURCE_DESC indexDesc = BufferDesc(indexBufferSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vertexBuffer)), "Create vertex buffer failed.");
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_indexBuffer)), "Create index buffer failed.");

    ComPtr<ID3D12Resource> vertexUpload;
    ComPtr<ID3D12Resource> indexUpload;
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexUpload)), "Create vertex upload failed.");
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &indexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&indexUpload)), "Create index upload failed.");

    void* mapped = nullptr;
    ThrowIfFailed(vertexUpload->Map(0, nullptr, &mapped), "Map vertex upload failed.");
    std::memcpy(mapped, vertices.data(), static_cast<std::size_t>(vertexBufferSize));
    vertexUpload->Unmap(0, nullptr);
    ThrowIfFailed(indexUpload->Map(0, nullptr, &mapped), "Map index upload failed.");
    std::memcpy(mapped, indices.data(), static_cast<std::size_t>(indexBufferSize));
    indexUpload->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "Command allocator reset failed.");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "Command list reset failed.");
    m_commandList->CopyBufferRegion(m_vertexBuffer.Get(), 0, vertexUpload.Get(), 0, vertexBufferSize);
    m_commandList->CopyBufferRegion(m_indexBuffer.Get(), 0, indexUpload.Get(), 0, indexBufferSize);
    D3D12_RESOURCE_BARRIER barriers[] =
    {
        Transition(m_vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        Transition(m_indexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    m_commandList->ResourceBarrier(_countof(barriers), barriers);
    ThrowIfFailed(m_commandList->Close(), "Geometry command list close failed.");
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForGpu();

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    m_vertexBufferView.StrideInBytes = sizeof(rb::SceneVertex);
    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

void D3D12Backend::ResetPreviewScene()
{
    std::vector<rb::SceneVertex> vertices =
    {
        { XMFLOAT3(-1,-1,-1), XMFLOAT3(0,0,-1), XMFLOAT2(0,1), XMFLOAT4(1,0,0,1) },
        { XMFLOAT3(-1, 1,-1), XMFLOAT3(0,0,-1), XMFLOAT2(0,0), XMFLOAT4(1,0,0,1) },
        { XMFLOAT3( 1, 1,-1), XMFLOAT3(0,0,-1), XMFLOAT2(1,0), XMFLOAT4(1,0,0,1) },
        { XMFLOAT3( 1,-1,-1), XMFLOAT3(0,0,-1), XMFLOAT2(1,1), XMFLOAT4(1,0,0,1) },
        { XMFLOAT3(-1,-1, 1), XMFLOAT3(0,0,1), XMFLOAT2(1,1), XMFLOAT4(-1,0,0,1) },
        { XMFLOAT3( 1,-1, 1), XMFLOAT3(0,0,1), XMFLOAT2(0,1), XMFLOAT4(-1,0,0,1) },
        { XMFLOAT3( 1, 1, 1), XMFLOAT3(0,0,1), XMFLOAT2(0,0), XMFLOAT4(-1,0,0,1) },
        { XMFLOAT3(-1, 1, 1), XMFLOAT3(0,0,1), XMFLOAT2(1,0), XMFLOAT4(-1,0,0,1) },
    };
    std::vector<std::uint32_t> indices =
    {
        0,1,2, 0,2,3, 4,5,6, 4,6,7,
        1,7,6, 1,6,2, 0,3,5, 0,5,4,
        3,2,6, 3,6,5, 0,4,7, 0,7,1,
    };
    rb::SceneDraw draw;
    draw.indexCount = static_cast<std::uint32_t>(indices.size());
    draw.materialIndex = 0;
    m_boundsMin = XMFLOAT3(-1.0f, -1.0f, -1.0f);
    m_boundsMax = XMFLOAT3(1.0f, 1.0f, 1.0f);
    CreateGeometry(vertices, indices, { draw });
    m_materials.clear();
    RenderMaterial material;
    material.name = "Default Material";
    m_materials.push_back(material);
    ResetCameraToScene();
}

bool D3D12Backend::LoadSceneMesh(const rb::ImportedScene& scene, std::string& diagnostics)
{
    if (scene.vertices.empty() || scene.indices.empty() || scene.draws.empty())
    {
        diagnostics = "Scene has no renderable geometry.";
        return false;
    }

    m_boundsMin = scene.boundsMin;
    m_boundsMax = scene.boundsMax;
    CreateGeometry(scene.vertices, scene.indices, scene.draws);

    std::vector<rb::MaterialAssignment> assignments;
    assignments.reserve(scene.materials.size());
    for (const rb::SceneMaterial& material : scene.materials)
    {
        assignments.push_back(material.assignment);
    }
    SetMaterialAssignments(assignments);

    for (std::size_t materialIndex = 0; materialIndex < scene.materials.size() && materialIndex < m_materials.size(); ++materialIndex)
    {
        for (std::size_t slot = 0; slot < MaterialTextureSlotCount; ++slot)
        {
            const std::wstring path = SceneTexturePath(scene.materials[materialIndex], slot);
            if (!path.empty())
            {
                std::string textureDiagnostics;
                UpdateMaterialTextureSlot(scene.materials[materialIndex].assignment.materialName, static_cast<std::uint32_t>(slot), path, textureDiagnostics);
            }
        }
    }
    ResetCameraToScene();
    diagnostics = "Loaded scene mesh.";
    return true;
}

void D3D12Backend::CreateDefaultMaterialResources()
{
    CreateFallbackTexture();
    if (m_materials.empty())
    {
        RenderMaterial material;
        material.name = "Default Material";
        m_materials.push_back(material);
    }
    m_materialTextures.resize(std::max<std::size_t>(m_materials.size(), 1));
    for (std::size_t materialIndex = 0; materialIndex < m_materials.size(); ++materialIndex)
    {
        const UINT descriptorBase = MaterialSrvDescriptorStart + static_cast<UINT>(materialIndex) * MaterialTextureSlotCount;
        for (UINT slot = 0; slot < MaterialTextureSlotCount; ++slot)
        {
            CreateFallbackSrv(descriptorBase + slot);
        }
        m_materials[materialIndex].textureTableGpu = SrvGpuHandle(descriptorBase);
    }
}

void D3D12Backend::CreateFallbackTexture()
{
    const std::uint32_t pixels[] =
    {
        0xffffffffu, 0xff7f7f7fu,
        0xff7f7f7fu, 0xffffffffu,
    };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2;
    desc.Height = 2;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_fallbackTexture)), "Create fallback texture failed.");
    D3D12_SUBRESOURCE_DATA subresource = {};
    subresource.pData = pixels;
    subresource.RowPitch = sizeof(std::uint32_t) * 2;
    subresource.SlicePitch = subresource.RowPitch * 2;
    UploadTextureSubresources(m_fallbackTexture.Get(), { subresource });
}

void D3D12Backend::CreateFallbackSrv(UINT descriptorIndex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_fallbackTexture.Get(), &srvDesc, SrvCpuHandle(descriptorIndex));
}

bool D3D12Backend::CreateTextureFromFile(const std::wstring& path, UINT descriptorIndex, ComPtr<ID3D12Resource>& texture, std::string& diagnostics)
{
    try
    {
        ScratchImage image;
        TexMetadata metadata = {};
        const std::wstring extension = LowerExtension(path);
        HRESULT hr = E_FAIL;
        if (extension == L".dds")
        {
            hr = LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, &metadata, image);
        }
        else if (extension == L".hdr")
        {
            hr = LoadFromHDRFile(path.c_str(), &metadata, image);
        }
        else if (extension == L".tga")
        {
            hr = LoadFromTGAFile(path.c_str(), &metadata, image);
        }
        else
        {
            hr = LoadFromWICFile(path.c_str(), WIC_FLAGS_FORCE_RGB, &metadata, image);
        }
        if (FAILED(hr))
        {
            diagnostics = "DirectXTex load failed: " + HResultMessage(hr);
            return false;
        }

        ScratchImage converted;
        const ScratchImage* source = &image;
        if (metadata.format == DXGI_FORMAT_R32G32B32A32_FLOAT || metadata.format == DXGI_FORMAT_R16G16B16A16_FLOAT || metadata.format == DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            source = &image;
        }
        else
        {
            hr = Convert(image.GetImages(), image.GetImageCount(), metadata, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, 0.0f, converted);
            if (FAILED(hr))
            {
                diagnostics = "DirectXTex convert failed: " + HResultMessage(hr);
                return false;
            }
            source = &converted;
        }

        CreateTextureResourceFromScratchImage(*source, descriptorIndex, texture);
        diagnostics = "Loaded texture.";
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

void D3D12Backend::CreateTextureResourceFromScratchImage(const ScratchImage& image, UINT descriptorIndex, ComPtr<ID3D12Resource>& texture)
{
    const TexMetadata& metadata = image.GetMetadata();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(metadata.width);
    desc.Height = static_cast<UINT>(metadata.height);
    desc.DepthOrArraySize = static_cast<UINT16>(metadata.arraySize);
    desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
    desc.Format = metadata.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "Create texture resource failed.");

    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    subresources.reserve(image.GetImageCount());
    for (std::size_t i = 0; i < image.GetImageCount(); ++i)
    {
        const Image* subImage = image.GetImages() + i;
        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = subImage->pixels;
        subresource.RowPitch = static_cast<LONG_PTR>(subImage->rowPitch);
        subresource.SlicePitch = static_cast<LONG_PTR>(subImage->slicePitch);
        subresources.push_back(subresource);
    }
    UploadTextureSubresources(texture.Get(), subresources);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = metadata.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(metadata.mipLevels);
    m_device->CreateShaderResourceView(texture.Get(), &srvDesc, SrvCpuHandle(descriptorIndex));
}

void D3D12Backend::UploadTextureSubresources(ID3D12Resource* texture, const std::vector<D3D12_SUBRESOURCE_DATA>& subresources)
{
    const UINT subresourceCount = static_cast<UINT>(subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
    std::vector<UINT> rowCounts(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadSize = 0;
    const D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
    m_device->GetCopyableFootprints(&textureDesc, 0, subresourceCount, 0, layouts.data(), rowCounts.data(), rowSizes.data(), &uploadSize);

    ComPtr<ID3D12Resource> upload;
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadDesc = BufferDesc(uploadSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create texture upload buffer failed.");

    std::uint8_t* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map texture upload failed.");
    for (UINT i = 0; i < subresourceCount; ++i)
    {
        const auto& layout = layouts[i];
        const auto& source = subresources[i];
        auto* destination = mapped + layout.Offset;
        for (UINT y = 0; y < rowCounts[i]; ++y)
        {
            std::memcpy(destination + static_cast<std::size_t>(layout.Footprint.RowPitch) * y, static_cast<const std::uint8_t*>(source.pData) + static_cast<std::size_t>(source.RowPitch) * y, static_cast<std::size_t>(rowSizes[i]));
        }
    }
    upload->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "Command allocator reset failed.");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "Command list reset failed.");
    for (UINT i = 0; i < subresourceCount; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = layouts[i];
        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER barrier = Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_commandList->ResourceBarrier(1, &barrier);
    ThrowIfFailed(m_commandList->Close(), "Texture upload command list close failed.");
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForGpu();
}

void D3D12Backend::CreateConstantBuffer()
{
    const UINT64 bufferSize = AlignTo(sizeof(SceneConstants), 256);
    D3D12_RESOURCE_DESC desc = BufferDesc(bufferSize);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    ThrowIfFailed(m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)), "Create constant buffer failed.");
    ThrowIfFailed(m_constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_constantBufferMapped)), "Map constant buffer failed.");
}

void D3D12Backend::CreateIblResources()
{
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC irradianceDesc = {};
    irradianceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    irradianceDesc.Width = IblWidth;
    irradianceDesc.Height = IblHeight;
    irradianceDesc.DepthOrArraySize = 1;
    irradianceDesc.MipLevels = 1;
    irradianceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    irradianceDesc.SampleDesc.Count = 1;
    irradianceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &irradianceDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_irradianceTexture)), "Create irradiance texture failed.");

    D3D12_RESOURCE_DESC prefilterDesc = irradianceDesc;
    prefilterDesc.MipLevels = IblMipLevels;
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &prefilterDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_prefilterTexture)), "Create prefilter texture failed.");

    D3D12_RESOURCE_DESC brdfDesc = irradianceDesc;
    brdfDesc.Width = BrdfLutSize;
    brdfDesc.Height = BrdfLutSize;
    brdfDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &brdfDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_brdfLutTexture)), "Create BRDF LUT texture failed.");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_irradianceTexture.Get(), &srvDesc, SrvCpuHandle(IrradianceSrvDescriptorIndex));
    srvDesc.Texture2D.MipLevels = IblMipLevels;
    m_device->CreateShaderResourceView(m_prefilterTexture.Get(), &srvDesc, SrvCpuHandle(PrefilterSrvDescriptorIndex));
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_brdfLutTexture.Get(), &srvDesc, SrvCpuHandle(BrdfLutSrvDescriptorIndex));
}

bool D3D12Backend::GenerateIblMaps(std::string& diagnostics)
{
    if (!m_environmentTexture)
    {
        diagnostics = "No environment texture is loaded.";
        return false;
    }
    try
    {
        WaitForGpu();
        ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "Command allocator reset failed.");
        ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "Command list reset failed.");
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetComputeRootSignature(m_computeRootSignature.Get());
        m_commandList->SetPipelineState(m_iblPipelineState.Get());
        m_commandList->SetComputeRootDescriptorTable(1, SrvGpuHandle(EnvironmentSrvDescriptorIndex));
        DispatchIbl(0, m_irradianceTexture.Get(), 0, IblWidth, IblHeight, 0.0f);
        for (UINT mip = 0; mip < IblMipLevels; ++mip)
        {
            const UINT width = std::max(1u, IblWidth >> mip);
            const UINT height = std::max(1u, IblHeight >> mip);
            const float roughness = static_cast<float>(mip) / static_cast<float>(IblMipLevels - 1);
            DispatchIbl(1, m_prefilterTexture.Get(), mip, width, height, roughness);
        }
        DispatchIbl(2, m_brdfLutTexture.Get(), 0, BrdfLutSize, BrdfLutSize, 0.0f);
        ThrowIfFailed(m_commandList->Close(), "IBL command list close failed.");
        ID3D12CommandList* commandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, commandLists);
        WaitForGpu();
        diagnostics = "Generated split-sum IBL maps.";
        return true;
    }
    catch (const std::exception& ex)
    {
        diagnostics = ex.what();
        return false;
    }
}

void D3D12Backend::DispatchIbl(UINT mode, ID3D12Resource* output, UINT mipLevel, UINT width, UINT height, float roughness)
{
    D3D12_RESOURCE_BARRIER toUav = Transition(output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mipLevel);
    m_commandList->ResourceBarrier(1, &toUav);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = output->GetDesc().Format;
    uavDesc.Texture2D.MipSlice = mipLevel;
    m_device->CreateUnorderedAccessView(output, nullptr, &uavDesc, SrvCpuHandle(IblUavDescriptorIndex));

    IblConstants constants;
    constants.mode = mode;
    constants.width = width;
    constants.height = height;
    constants.mipLevel = mipLevel;
    constants.roughness = roughness;
    constants.sourceMipCount = static_cast<float>(m_environmentMipLevels);
    m_commandList->SetComputeRoot32BitConstants(0, sizeof(IblConstants) / sizeof(std::uint32_t), &constants, 0);
    m_commandList->SetComputeRootDescriptorTable(2, SrvGpuHandle(IblUavDescriptorIndex));
    m_commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = output;
    m_commandList->ResourceBarrier(1, &uavBarrier);
    D3D12_RESOURCE_BARRIER toSrv = Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, mipLevel);
    m_commandList->ResourceBarrier(1, &toSrv);
}

bool D3D12Backend::UpdateEnvironmentTexture(const std::wstring& path, std::string& diagnostics)
{
    if (path.empty())
    {
        m_environmentTexture.Reset();
        m_environmentMipLevels = 1;
        m_lookDevEnvironment.environmentPath.clear();
        m_lookDevConstants.environmentOptions.w = 0.0f;
        m_environmentStatus = "Using SkyColor background.";
        diagnostics = m_environmentStatus;
        return true;
    }

    ComPtr<ID3D12Resource> texture;
    std::string textureDiagnostics;
    if (!CreateTextureFromFile(path, EnvironmentSrvDescriptorIndex, texture, textureDiagnostics))
    {
        diagnostics = textureDiagnostics;
        return false;
    }

    m_environmentTexture = texture;
    m_environmentMipLevels = std::max<UINT>(texture->GetDesc().MipLevels, 1u);
    m_lookDevEnvironment.environmentPath = path;
    m_lookDevConstants.environmentOptions.w = 1.0f;
    m_lookDevConstants.iblOptions.x = static_cast<float>(IblMipLevels - 1);
    std::string iblDiagnostics;
    GenerateIblMaps(iblDiagnostics);
    m_environmentStatus = "Loaded " + std::filesystem::path(path).filename().string();
    diagnostics = m_environmentStatus + "\n" + textureDiagnostics + "\n" + iblDiagnostics;
    return true;
}

bool D3D12Backend::UpdateMaterialTextureSlot(const std::string& materialName, std::uint32_t textureSlot, const std::wstring& path, std::string& diagnostics)
{
    auto it = std::find_if(m_materials.begin(), m_materials.end(), [&](const RenderMaterial& material) {
        return material.name == materialName;
    });
    if (it == m_materials.end())
    {
        diagnostics = "Material not found.";
        return false;
    }
    if (textureSlot >= MaterialTextureSlotCount)
    {
        diagnostics = "Texture slot out of range.";
        return false;
    }
    const std::size_t materialIndex = static_cast<std::size_t>(std::distance(m_materials.begin(), it));
    const UINT descriptorIndex = MaterialSrvDescriptorStart + static_cast<UINT>(materialIndex) * MaterialTextureSlotCount + textureSlot;
    if (path.empty())
    {
        CreateFallbackSrv(descriptorIndex);
        it->texturePaths[textureSlot].clear();
        it->constants.textureMask &= ~(1u << textureSlot);
        diagnostics = "Texture slot cleared.";
        return true;
    }

    ComPtr<ID3D12Resource> texture;
    if (!CreateTextureFromFile(path, descriptorIndex, texture, diagnostics))
    {
        return false;
    }
    if (materialIndex >= m_materialTextures.size())
    {
        m_materialTextures.resize(materialIndex + 1);
    }
    m_materialTextures[materialIndex][textureSlot] = texture;
    it->texturePaths[textureSlot] = path;
    it->constants.textureMask |= 1u << textureSlot;
    return true;
}

void D3D12Backend::SetMaterialAssignments(const std::vector<rb::MaterialAssignment>& assignments)
{
    m_materials.clear();
    m_materialTextures.clear();
    const std::size_t count = std::min<std::size_t>(assignments.empty() ? 1 : assignments.size(), MaxMaterialCount);
    m_materials.resize(count);
    m_materialTextures.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const rb::MaterialAssignment assignment = assignments.empty() ? rb::MaterialAssignment{} : assignments[i];
        RenderMaterial& material = m_materials[i];
        material.name = assignment.materialName.empty() ? "Default Material" : assignment.materialName;
        material.constants.baseColorFactor = XMFLOAT4(assignment.baseColorFactor[0], assignment.baseColorFactor[1], assignment.baseColorFactor[2], assignment.baseColorFactor[3]);
        material.constants.emissiveFactor = XMFLOAT4(assignment.emissiveFactor[0], assignment.emissiveFactor[1], assignment.emissiveFactor[2], assignment.emissiveFactor[3]);
        material.constants.roughnessFactor = assignment.roughnessFactor;
        material.constants.metallicFactor = assignment.metallicFactor;
        material.constants.occlusionStrength = assignment.occlusionStrength;
        material.constants.normalStrength = assignment.normalStrength;
        material.constants.normalGreenScale = assignment.flipNormalGreen ? -1.0f : 1.0f;
        material.constants.alphaCutoff = assignment.alphaCutoff;
        material.constants.alphaMode = static_cast<float>(assignment.alphaMode);
        material.constants.packedOcclusionRoughnessMetallic = assignment.packedOcclusionRoughnessMetallic ? 1.0f : 0.0f;
        UINT mask = 0;
        const UINT descriptorBase = MaterialSrvDescriptorStart + static_cast<UINT>(i) * MaterialTextureSlotCount;
        for (UINT slot = 0; slot < MaterialTextureSlotCount; ++slot)
        {
            CreateFallbackSrv(descriptorBase + slot);
            if (assignment.textureOverrideEnabled[slot] && !assignment.textureOverrides[slot].empty())
            {
                std::string diagnostics;
                UpdateMaterialTextureSlot(material.name, slot, assignment.textureOverrides[slot], diagnostics);
            }
            if (!material.texturePaths[slot].empty())
            {
                mask |= 1u << slot;
            }
        }
        if (!material.texturePaths[TextureSlotBaseColor].empty()) { mask |= MaterialTextureBaseColorBit; }
        if (!material.texturePaths[TextureSlotNormal].empty()) { mask |= MaterialTextureNormalBit; }
        if (!material.texturePaths[TextureSlotRoughness].empty()) { mask |= MaterialTextureRoughnessBit; }
        if (!material.texturePaths[TextureSlotMetallic].empty()) { mask |= MaterialTextureMetallicBit; }
        if (!material.texturePaths[TextureSlotOcclusion].empty()) { mask |= MaterialTextureOcclusionBit; }
        if (!material.texturePaths[TextureSlotEmissive].empty()) { mask |= MaterialTextureEmissiveBit; }
        material.constants.textureMask = mask;
        material.textureTableGpu = SrvGpuHandle(descriptorBase);
    }
}

void D3D12Backend::SetSkyColors(const std::array<float, 4>& topColor, const std::array<float, 4>& horizonColor)
{
    m_lookDevConstants.skyTopColor = XMFLOAT4(topColor[0], topColor[1], topColor[2], topColor[3]);
    m_lookDevConstants.skyHorizonColor = XMFLOAT4(horizonColor[0], horizonColor[1], horizonColor[2], horizonColor[3]);
}

void D3D12Backend::SetLookDevEnvironment(const rb::LookDevEnvironment& environment)
{
    m_lookDevEnvironment = environment;
    const float illuminance = environment.sunIntensity > 100.0f ? environment.sunIntensity : environment.sunIntensity * 10000.0f;
    m_lookDevConstants.sunColorIntensity = XMFLOAT4(environment.sunColor[0], environment.sunColor[1], environment.sunColor[2], illuminance / 10000.0f);
    m_lookDevConstants.environmentOptions.x = environment.rotationYaw;
    m_lookDevConstants.environmentOptions.y = environment.intensity;
    m_lookDevConstants.environmentOptions.z = static_cast<float>(environment.backgroundMode);
    m_lookDevConstants.iblOptions.w = 1.0f;
}

void D3D12Backend::SetLookDevViewSettings(const rb::LookDevViewSettings& settings)
{
    m_lookDevViewSettings = settings;
    m_lookDevConstants.viewOptions.x = settings.exposure;
    m_lookDevConstants.viewOptions.y = settings.gamma;
    m_lookDevConstants.viewOptions.z = static_cast<float>(settings.toneMapper);
    m_lookDevConstants.viewOptions.w = static_cast<float>(settings.displayMode);
}

void D3D12Backend::SetDebugViewMode(rb::LookDevDisplayMode displayMode)
{
    m_lookDevViewSettings.displayMode = displayMode;
    m_lookDevConstants.viewOptions.w = static_cast<float>(displayMode);
}

void D3D12Backend::SetModelTransform(const rb::ModelTransform& transform)
{
    m_modelTransform = transform;
}

bool D3D12Backend::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0)
    {
        return false;
    }
    WaitForGpu();
    ReleaseRenderTargets();
    m_width = width;
    m_height = height;
    ThrowIfFailed(m_swapChain->ResizeBuffers(FrameCount, width, height, m_backBufferFormat, 0), "ResizeBuffers failed.");
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTargets();
    return true;
}

bool D3D12Backend::ResizeSceneTarget(UINT width, UINT height)
{
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (width == m_sceneWidth && height == m_sceneHeight)
    {
        return true;
    }
    WaitForGpu();
    m_sceneWidth = width;
    m_sceneHeight = height;
    m_sceneTarget.Reset();
    m_sceneDepth.Reset();
    CreateSceneTarget();
    return true;
}

void D3D12Backend::UpdateConstants(float deltaSeconds)
{
    m_elapsedSeconds += deltaSeconds;
    if (m_lookDevViewSettings.turntableEnabled)
    {
        OrbitCamera(m_lookDevViewSettings.turntableSpeed * deltaSeconds, 0.0f);
    }

    const float aspect = static_cast<float>(m_sceneWidth) / static_cast<float>(std::max(m_sceneHeight, 1u));
    const XMVECTOR target = XMLoadFloat3(&m_cameraTarget);
    const float distance = std::max(m_cameraDistance, 0.01f);
    const float cp = std::cos(m_cameraPitch);
    const XMVECTOR offset = XMVectorSet(std::sin(m_cameraYaw) * cp * distance, std::sin(m_cameraPitch) * distance, std::cos(m_cameraYaw) * cp * distance, 0.0f);
    const XMVECTOR eye = target + offset;
    const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(45.0f), aspect, 0.01f, 100000.0f);
    const XMMATRIX model = ModelMatrix();
    const XMMATRIX viewProjection = view * projection;
    XMStoreFloat4x4(&m_sceneConstants.modelViewProjection, XMMatrixTranspose(model * viewProjection));
    XMStoreFloat4x4(&m_sceneConstants.model, XMMatrixTranspose(model));
    XMStoreFloat4x4(&m_sceneConstants.viewProjectionInverse, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
    XMFLOAT3 eyeFloat;
    XMStoreFloat3(&eyeFloat, eye);
    m_sceneConstants.cameraPositionTime = XMFLOAT4(eyeFloat.x, eyeFloat.y, eyeFloat.z, m_elapsedSeconds);
    m_sceneConstants.lightDirectionIntensity = XMFLOAT4(
        m_lookDevEnvironment.sunDirection[0],
        m_lookDevEnvironment.sunDirection[1],
        m_lookDevEnvironment.sunDirection[2],
        1.0f);
    XMStoreFloat4x4(&m_sceneConstants.shadowViewProjection, XMMatrixIdentity());
    std::memcpy(m_constantBufferMapped, &m_sceneConstants, sizeof(m_sceneConstants));
}

void D3D12Backend::DrawSky()
{
    m_commandList->SetGraphicsRootSignature(m_graphicsRootSignature.Get());
    m_commandList->SetPipelineState(m_skyPipelineState.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRoot32BitConstants(3, sizeof(LookDevConstants) / sizeof(std::uint32_t), &m_lookDevConstants, 0);
    m_commandList->SetGraphicsRootDescriptorTable(4, SrvGpuHandle(EnvironmentSrvDescriptorIndex));
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 0, nullptr);
    m_commandList->IASetIndexBuffer(nullptr);
    m_commandList->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::DrawScene()
{
    if (!m_pbrPipelineState || !m_vertexBuffer || !m_indexBuffer || m_materials.empty())
    {
        return;
    }
    m_commandList->SetGraphicsRootSignature(m_graphicsRootSignature.Get());
    m_commandList->SetPipelineState(m_pbrPipelineState.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRoot32BitConstants(3, sizeof(LookDevConstants) / sizeof(std::uint32_t), &m_lookDevConstants, 0);
    m_commandList->SetGraphicsRootDescriptorTable(4, SrvGpuHandle(EnvironmentSrvDescriptorIndex));
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);

    for (const rb::SceneDraw& draw : m_draws)
    {
        if (draw.indexCount == 0)
        {
            continue;
        }
        const RenderMaterial& material = m_materials[std::min<std::size_t>(draw.materialIndex, m_materials.size() - 1)];
        m_commandList->SetGraphicsRootDescriptorTable(1, material.textureTableGpu);
        m_commandList->SetGraphicsRoot32BitConstants(2, sizeof(MaterialConstants) / sizeof(std::uint32_t), &material.constants, 0);
        m_commandList->DrawIndexedInstanced(draw.indexCount, 1, draw.startIndex, draw.baseVertex, 0);
    }
}

void D3D12Backend::Render(float deltaSeconds)
{
    if (!m_device || !m_sceneTarget)
    {
        return;
    }
    const UINT64 frameFenceValue = m_fenceValues[m_frameIndex];
    if (frameFenceValue != 0 && m_fence && m_fence->GetCompletedValue() < frameFenceValue)
    {
        ImGui::Render();
        return;
    }

    const auto startTime = std::chrono::high_resolution_clock::now();
    UpdateConstants(deltaSeconds);
    ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset(), "Command allocator reset failed.");
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr), "Command list reset failed.");

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);

    if (m_sceneTargetState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER barrier = Transition(m_sceneTarget.Get(), m_sceneTargetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &barrier);
        m_sceneTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = RtvHandle(FrameCount);
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv = DsvHandle(0);
    const float clearColor[] = { m_lookDevConstants.skyHorizonColor.x, m_lookDevConstants.skyHorizonColor.y, m_lookDevConstants.skyHorizonColor.z, 1.0f };
    m_commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, &sceneDsv);
    m_commandList->ClearRenderTargetView(sceneRtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(sceneDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT sceneViewport = { 0.0f, 0.0f, static_cast<float>(m_sceneWidth), static_cast<float>(m_sceneHeight), 0.0f, 1.0f };
    D3D12_RECT sceneScissor = { 0, 0, static_cast<LONG>(m_sceneWidth), static_cast<LONG>(m_sceneHeight) };
    m_commandList->RSSetViewports(1, &sceneViewport);
    m_commandList->RSSetScissorRects(1, &sceneScissor);
    DrawSky();
    DrawScene();

    D3D12_RESOURCE_BARRIER sceneToSrv = Transition(m_sceneTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_commandList->ResourceBarrier(1, &sceneToSrv);
    m_sceneTargetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_RESOURCE_BARRIER backBufferToRtv = Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &backBufferToRtv);
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = RtvHandle(m_frameIndex);
    const float backClear[] = { 0.015f, 0.017f, 0.02f, 1.0f };
    m_commandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(backBufferRtv, backClear, 0, nullptr);

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

    D3D12_RESOURCE_BARRIER backBufferToPresent = Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &backBufferToPresent);
    ThrowIfFailed(m_commandList->Close(), "Command list close failed.");

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    ThrowIfFailed(m_swapChain->Present(1, 0), "Present failed.");
    MoveToNextFrame();

    const auto endTime = std::chrono::high_resolution_clock::now();
    m_lastFrameMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    ++m_frameNumber;
}

void D3D12Backend::InitializeImGui(HWND hwnd)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    m_imguiDefaultIniFile = (m_rootDirectory / "imgui.ini").lexically_normal();
    m_imguiUserIniFile = (m_rootDirectory / "imgui.user.ini").lexically_normal();
    m_imguiDefaultIniPath = PathToUtf8(m_imguiDefaultIniFile);
    m_imguiUserIniPath = PathToUtf8(m_imguiUserIniFile);
    io.IniFilename = m_imguiUserIniPath.c_str();
    std::error_code ec;
    ImGui::LoadIniSettingsFromDisk(std::filesystem::exists(m_imguiUserIniFile, ec) ? m_imguiUserIniPath.c_str() : m_imguiDefaultIniPath.c_str());
    LoadJapaneseImGuiFonts(io);
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_InitInfo initInfo;
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_commandQueue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = m_backBufferFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = m_srvHeap.Get();
    initInfo.UserData = this;
    initInfo.SrvDescriptorAllocFn = &D3D12Backend::ImGuiSrvAllocate;
    initInfo.SrvDescriptorFreeFn = &D3D12Backend::ImGuiSrvFree;
    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        throw std::runtime_error("ImGui_ImplDX12_Init failed.");
    }
    m_imguiInitialized = true;
}

bool D3D12Backend::ResetImGuiLayout()
{
    if (!m_imguiInitialized || m_imguiDefaultIniPath.empty() || m_imguiUserIniPath.empty())
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(m_imguiUserIniFile, ec);
    ImGui::ClearIniSettings();
    ImGui::LoadIniSettingsFromDisk(m_imguiDefaultIniPath.c_str());
    ImGui::SaveIniSettingsToDisk(m_imguiUserIniPath.c_str());
    return true;
}

void D3D12Backend::ImGuiSrvAllocate(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    auto* backend = static_cast<D3D12Backend*>(info->UserData);
    const UINT index = backend->m_srvAllocator.used++;
    *cpu = backend->SrvCpuHandle(index);
    *gpu = backend->SrvGpuHandle(index);
}

void D3D12Backend::ImGuiSrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
{
}

bool D3D12Backend::TryWaitForGpu(DWORD timeoutMs)
{
    if (!m_commandQueue || !m_fence)
    {
        return true;
    }
    const UINT64 fenceValue = m_nextFenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue), "Fence signal failed.");
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent), "SetEventOnCompletion failed.");
        if (WaitForSingleObjectEx(m_fenceEvent, timeoutMs, FALSE) == WAIT_TIMEOUT)
        {
            return false;
        }
    }
    m_fenceValues.fill(0);
    return true;
}

void D3D12Backend::WaitForGpu()
{
    if (!TryWaitForGpu(5000))
    {
        throw std::runtime_error("GPU wait timed out.");
    }
}

void D3D12Backend::MoveToNextFrame()
{
    const UINT submittedFrameIndex = m_frameIndex;
    const UINT64 fenceValue = m_nextFenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValue), "Fence signal failed.");
    m_fenceValues[submittedFrameIndex] = fenceValue;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

float D3D12Backend::SceneRadius() const
{
    XMFLOAT3 boundsMin;
    XMFLOAT3 boundsMax;
    TransformedSceneBounds(boundsMin, boundsMax);
    const XMVECTOR minBounds = XMLoadFloat3(&boundsMin);
    const XMVECTOR maxBounds = XMLoadFloat3(&boundsMax);
    return std::max(0.5f, XMVectorGetX(XMVector3Length(maxBounds - minBounds)) * 0.5f);
}

void D3D12Backend::ResetCameraToScene()
{
    XMFLOAT3 boundsMin;
    XMFLOAT3 boundsMax;
    TransformedSceneBounds(boundsMin, boundsMax);
    m_cameraTarget = XMFLOAT3(
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f);
    m_cameraDistance = SceneRadius() * 2.8f;
    m_cameraMoveScale = SceneRadius();
}

XMMATRIX D3D12Backend::ModelMatrix() const
{
    const XMMATRIX rotation = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(m_modelTransform.rotationDegrees[0]),
        XMConvertToRadians(m_modelTransform.rotationDegrees[1]),
        XMConvertToRadians(m_modelTransform.rotationDegrees[2]));
    const XMMATRIX translation = XMMatrixTranslation(
        m_modelTransform.translation[0],
        m_modelTransform.translation[1],
        m_modelTransform.translation[2]);
    return rotation * translation;
}

void D3D12Backend::TransformedSceneBounds(XMFLOAT3& boundsMin, XMFLOAT3& boundsMax) const
{
    const XMMATRIX model = ModelMatrix();
    const XMFLOAT3 corners[] =
    {
        XMFLOAT3(m_boundsMin.x, m_boundsMin.y, m_boundsMin.z),
        XMFLOAT3(m_boundsMin.x, m_boundsMin.y, m_boundsMax.z),
        XMFLOAT3(m_boundsMin.x, m_boundsMax.y, m_boundsMin.z),
        XMFLOAT3(m_boundsMin.x, m_boundsMax.y, m_boundsMax.z),
        XMFLOAT3(m_boundsMax.x, m_boundsMin.y, m_boundsMin.z),
        XMFLOAT3(m_boundsMax.x, m_boundsMin.y, m_boundsMax.z),
        XMFLOAT3(m_boundsMax.x, m_boundsMax.y, m_boundsMin.z),
        XMFLOAT3(m_boundsMax.x, m_boundsMax.y, m_boundsMax.z),
    };

    XMFLOAT3 transformed;
    XMStoreFloat3(&transformed, XMVector3Transform(XMLoadFloat3(&corners[0]), model));
    boundsMin = transformed;
    boundsMax = transformed;
    for (std::size_t i = 1; i < _countof(corners); ++i)
    {
        XMStoreFloat3(&transformed, XMVector3Transform(XMLoadFloat3(&corners[i]), model));
        boundsMin.x = std::min(boundsMin.x, transformed.x);
        boundsMin.y = std::min(boundsMin.y, transformed.y);
        boundsMin.z = std::min(boundsMin.z, transformed.z);
        boundsMax.x = std::max(boundsMax.x, transformed.x);
        boundsMax.y = std::max(boundsMax.y, transformed.y);
        boundsMax.z = std::max(boundsMax.z, transformed.z);
    }
}

rb::ViewportCamera D3D12Backend::CameraState() const
{
    rb::ViewportCamera camera;
    camera.target = { m_cameraTarget.x, m_cameraTarget.y, m_cameraTarget.z };
    camera.yaw = m_cameraYaw;
    camera.pitch = m_cameraPitch;
    camera.distance = m_cameraDistance;
    return camera;
}

void D3D12Backend::SetCameraState(const rb::ViewportCamera& camera)
{
    m_cameraTarget = XMFLOAT3(camera.target[0], camera.target[1], camera.target[2]);
    m_cameraYaw = camera.yaw;
    m_cameraPitch = std::clamp(camera.pitch, -1.55f, 1.55f);
    m_cameraDistance = std::max(camera.distance, 0.01f);
}

void D3D12Backend::OrbitCamera(float yawDeltaRadians, float pitchDeltaRadians)
{
    m_cameraYaw += yawDeltaRadians;
    m_cameraPitch = std::clamp(m_cameraPitch + pitchDeltaRadians, -1.55f, 1.55f);
}

void D3D12Backend::CameraBasis(XMVECTOR& forward, XMVECTOR& right, XMVECTOR& up) const
{
    const float cp = std::cos(m_cameraPitch);
    forward = XMVector3Normalize(XMVectorSet(-std::sin(m_cameraYaw) * cp, -std::sin(m_cameraPitch), -std::cos(m_cameraYaw) * cp, 0.0f));
    right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
    up = XMVector3Normalize(XMVector3Cross(forward, right));
}

void D3D12Backend::PanCamera(float rightDelta, float upDelta)
{
    XMVECTOR forward;
    XMVECTOR right;
    XMVECTOR up;
    CameraBasis(forward, right, up);
    XMVECTOR target = XMLoadFloat3(&m_cameraTarget);
    const float scale = m_cameraDistance * 0.5f;
    target += (-right * rightDelta + up * upDelta) * scale;
    XMStoreFloat3(&m_cameraTarget, target);
}

void D3D12Backend::DollyCamera(float wheelDelta)
{
    m_cameraDistance *= std::pow(0.88f, wheelDelta);
    m_cameraDistance = std::max(0.01f, m_cameraDistance);
}

void D3D12Backend::MoveCamera(float forwardDelta, float rightDelta, float upDelta)
{
    XMVECTOR forward;
    XMVECTOR right;
    XMVECTOR up;
    CameraBasis(forward, right, up);
    XMVECTOR target = XMLoadFloat3(&m_cameraTarget);
    const float scale = std::max(m_cameraMoveScale, 0.1f);
    target += (forward * forwardDelta + right * rightDelta + up * upDelta) * scale;
    XMStoreFloat3(&m_cameraTarget, target);
}

RendererStats D3D12Backend::Stats() const
{
    RendererStats stats;
    stats.adapterName = m_adapterName;
    stats.shaderModel69 = m_shaderModel69;
    stats.lastFrameMs = m_lastFrameMs;
    stats.frameNumber = m_frameNumber;
    stats.sceneWidth = m_sceneWidth;
    stats.sceneHeight = m_sceneHeight;
    stats.vertexCount = m_vertexCount;
    stats.indexCount = m_indexCount;
    stats.environmentStatus = m_environmentStatus;
    return stats;
}
}
