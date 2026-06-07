#pragma once

#include "EditorTypes.h"
#include "SceneTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <DirectXMath.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ImGui_ImplDX12_InitInfo;

namespace DirectX
{
class ScratchImage;
}

namespace cld
{
struct RendererStats
{
    std::string adapterName;
    bool shaderModel69 = false;
    double lastFrameMs = 0.0;
    std::uint64_t frameNumber = 0;
    std::uint32_t sceneWidth = 0;
    std::uint32_t sceneHeight = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::string environmentStatus;
};

class D3D12Backend
{
public:
    static constexpr UINT FrameCount = 3;

    D3D12Backend() = default;
    ~D3D12Backend();

    D3D12Backend(const D3D12Backend&) = delete;
    D3D12Backend& operator=(const D3D12Backend&) = delete;

    void Initialize(HWND hwnd, UINT width, UINT height, const std::filesystem::path& rootDirectory);
    void Shutdown();
    bool Resize(UINT width, UINT height);
    bool ResizeSceneTarget(UINT width, UINT height);
    void Render(float deltaSeconds);

    bool LoadSceneMesh(const rb::ImportedScene& scene, std::string& diagnostics);
    void ResetPreviewScene();
    void ResetCameraToScene();
    bool UpdateEnvironmentTexture(const std::wstring& path, std::string& diagnostics);
    bool UpdateMaterialTextureSlot(const std::string& materialName, std::uint32_t textureSlot, const std::wstring& path, std::string& diagnostics);
    void SetMaterialAssignments(const std::vector<rb::MaterialAssignment>& assignments);
    void SetSkyColors(const std::array<float, 4>& topColor, const std::array<float, 4>& horizonColor);
    void SetLookDevEnvironment(const rb::LookDevEnvironment& environment);
    void SetLookDevViewSettings(const rb::LookDevViewSettings& settings);
    void SetDebugViewMode(rb::LookDevDisplayMode displayMode);
    void SetModelTransform(const rb::ModelTransform& transform);
    bool ResetImGuiLayout();

    rb::ViewportCamera CameraState() const;
    void SetCameraState(const rb::ViewportCamera& camera);
    void OrbitCamera(float yawDeltaRadians, float pitchDeltaRadians);
    void PanCamera(float rightDelta, float upDelta);
    void DollyCamera(float wheelDelta);
    void MoveCamera(float forwardDelta, float rightDelta, float upDelta);

    ID3D12Device* Device() const { return m_device.Get(); }
    ID3D12CommandQueue* CommandQueue() const { return m_commandQueue.Get(); }
    ID3D12DescriptorHeap* SrvHeap() const { return m_srvHeap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE SceneSrvGpu() const { return m_sceneSrvGpu; }
    UINT SceneWidth() const { return m_sceneWidth; }
    UINT SceneHeight() const { return m_sceneHeight; }
    RendererStats Stats() const;

private:
    struct SceneConstants
    {
        DirectX::XMFLOAT4X4 modelViewProjection;
        DirectX::XMFLOAT4X4 model;
        DirectX::XMFLOAT4X4 viewProjectionInverse;
        DirectX::XMFLOAT4 cameraPositionTime;
        DirectX::XMFLOAT4 lightDirectionIntensity;
        DirectX::XMFLOAT4X4 shadowViewProjection;
    };

    struct MaterialConstants
    {
        DirectX::XMFLOAT4 baseColorFactor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        UINT textureMask = 0;
        float normalStrength = 1.0f;
        float normalGreenScale = 1.0f;
        float roughnessFactor = 0.48f;
        float metallicFactor = 0.0f;
        float occlusionStrength = 1.0f;
        float alphaCutoff = 0.5f;
        float alphaMode = 0.0f;
        DirectX::XMFLOAT4 emissiveFactor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        float packedOcclusionRoughnessMetallic = 0.0f;
    };

    struct LookDevConstants
    {
        DirectX::XMFLOAT4 sunColorIntensity = DirectX::XMFLOAT4(1.0f, 0.96f, 0.88f, 10000.0f);
        DirectX::XMFLOAT4 environmentOptions = DirectX::XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);
        DirectX::XMFLOAT4 viewOptions = DirectX::XMFLOAT4(0.0f, 2.2f, 2.0f, 0.0f);
        DirectX::XMFLOAT4 iblOptions = DirectX::XMFLOAT4(5.0f, 1.0f, 1.0f, 1.0f);
        DirectX::XMFLOAT4 skyTopColor = DirectX::XMFLOAT4(0.12f, 0.22f, 0.36f, 1.0f);
        DirectX::XMFLOAT4 skyHorizonColor = DirectX::XMFLOAT4(0.035f, 0.045f, 0.055f, 1.0f);
        DirectX::XMFLOAT4 shadowOptions = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    };

    struct IblConstants
    {
        UINT mode = 0;
        UINT width = 0;
        UINT height = 0;
        UINT mipLevel = 0;
        float roughness = 0.0f;
        float sourceMipCount = 1.0f;
        float padding[2] = {};
    };

    struct RenderMaterial
    {
        std::string name = "Default Material";
        std::array<std::wstring, static_cast<std::size_t>(rb::TextureSlot::Count)> texturePaths;
        MaterialConstants constants;
        D3D12_GPU_DESCRIPTOR_HANDLE textureTableGpu = {};
    };

    struct SrvAllocator
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = {};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = {};
        UINT descriptorSize = 0;
        UINT used = 0;
    };

    static void ImGuiSrvAllocate(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    static void ImGuiSrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

    void CreateDeviceObjects(HWND hwnd, UINT width, UINT height);
    void CreateRenderTargets();
    void ReleaseRenderTargets();
    void CreateSceneTarget();
    void CreateRootSignatures();
    void CreatePipelineStates();
    void CreateGeometry(const std::vector<rb::SceneVertex>& vertices, const std::vector<std::uint32_t>& indices, const std::vector<rb::SceneDraw>& draws);
    void CreateDefaultMaterialResources();
    void CreateFallbackTexture();
    void CreateFallbackSrv(UINT descriptorIndex);
    bool CreateTextureFromFile(const std::wstring& path, UINT descriptorIndex, Microsoft::WRL::ComPtr<ID3D12Resource>& texture, std::string& diagnostics);
    void CreateTextureResourceFromScratchImage(const DirectX::ScratchImage& image, UINT descriptorIndex, Microsoft::WRL::ComPtr<ID3D12Resource>& texture);
    void UploadTextureSubresources(ID3D12Resource* texture, const std::vector<D3D12_SUBRESOURCE_DATA>& subresources);
    void CreateConstantBuffer();
    void CreateIblResources();
    bool GenerateIblMaps(std::string& diagnostics);
    void DispatchIbl(UINT mode, ID3D12Resource* output, UINT mipLevel, UINT width, UINT height, float roughness);
    void InitializeImGui(HWND hwnd);
    void WaitForGpu();
    bool TryWaitForGpu(DWORD timeoutMs);
    void MoveToNextFrame();
    void UpdateConstants(float deltaSeconds);
    void DrawSky();
    void DrawScene();
    DirectX::XMMATRIX ModelMatrix() const;
    void TransformedSceneBounds(DirectX::XMFLOAT3& boundsMin, DirectX::XMFLOAT3& boundsMax) const;
    void CameraBasis(DirectX::XMVECTOR& forward, DirectX::XMVECTOR& right, DirectX::XMVECTOR& up) const;
    float SceneRadius() const;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle(UINT index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(UINT index) const;
    static std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path);
    void ThrowIfFailed(HRESULT hr, const char* message) const;

    std::filesystem::path m_rootDirectory;
    std::filesystem::path m_imguiDefaultIniFile;
    std::filesystem::path m_imguiUserIniFile;
    std::string m_imguiDefaultIniPath;
    std::string m_imguiUserIniPath;
    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_sceneWidth = 1280;
    UINT m_sceneHeight = 720;
    DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT m_sceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_FORMAT m_sceneDepthFormat = DXGI_FORMAT_D32_FLOAT;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, FrameCount> m_commandAllocators;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_renderTargets;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    std::array<UINT64, FrameCount> m_fenceValues = {};
    UINT64 m_nextFenceValue = 1;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_dsvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_graphicsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pbrPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_skyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_iblPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fallbackTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_environmentTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_irradianceTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_prefilterTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_brdfLutTexture;
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, static_cast<std::size_t>(rb::TextureSlot::Count)>> m_materialTextures;

    std::vector<RenderMaterial> m_materials;
    std::vector<rb::SceneDraw> m_draws;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    UINT m_vertexCount = 0;
    UINT m_indexCount = 0;
    std::uint8_t* m_constantBufferMapped = nullptr;
    D3D12_RESOURCE_STATES m_sceneTargetState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_CPU_DESCRIPTOR_HANDLE m_sceneSrvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_sceneSrvGpu = {};
    SrvAllocator m_srvAllocator;

    std::string m_adapterName = "Unknown";
    bool m_shaderModel69 = false;
    bool m_imguiInitialized = false;
    double m_lastFrameMs = 0.0;
    UINT64 m_frameNumber = 0;
    float m_elapsedSeconds = 0.0f;
    DirectX::XMFLOAT3 m_boundsMin = DirectX::XMFLOAT3(-1.0f, -1.0f, -1.0f);
    DirectX::XMFLOAT3 m_boundsMax = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
    DirectX::XMFLOAT3 m_cameraTarget = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    float m_cameraYaw = 0.0f;
    float m_cameraPitch = 0.12f;
    float m_cameraDistance = 4.0f;
    float m_cameraMoveScale = 1.0f;
    SceneConstants m_sceneConstants = {};
    LookDevConstants m_lookDevConstants = {};
    rb::ModelTransform m_modelTransform;
    rb::LookDevEnvironment m_lookDevEnvironment;
    rb::LookDevViewSettings m_lookDevViewSettings;
    std::string m_environmentStatus = "Using SkyColor background.";
    UINT m_environmentMipLevels = 1;
};
}
