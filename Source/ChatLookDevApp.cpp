#include "ChatLookDevApp.h"

#include "SimpleJson.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <commdlg.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace
{
constexpr const char* TextureSlotLabels[] =
{
    "Base Color",
    "Normal",
    "Roughness",
    "Metallic",
    "Occlusion",
    "Emissive",
};

constexpr const char* TextureSlotJsonNames[] =
{
    "baseColor",
    "normal",
    "roughness",
    "metallic",
    "occlusion",
    "emissive",
};

constexpr std::size_t TextureSlotCount = static_cast<std::size_t>(rb::TextureSlot::Count);

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t ch : text)
        {
            fallback.push_back(static_cast<char>(ch & 0x7f));
        }
        return fallback;
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.c_str()), text.size());
}

std::filesystem::path Utf8ToPath(const std::string& text)
{
    return std::filesystem::path(Utf8ToWide(text));
}

std::filesystem::path AbsoluteLexicalPath(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
    {
        absolute = path;
    }
    return absolute.lexically_normal();
}

std::filesystem::path ResolveProjectPath(const std::string& text, const std::filesystem::path& projectDirectory)
{
    if (text.empty())
    {
        return {};
    }
    std::filesystem::path path = Utf8ToPath(text);
    if (path.is_absolute())
    {
        return AbsoluteLexicalPath(path);
    }
    return AbsoluteLexicalPath(projectDirectory / path);
}

std::string ProjectPathString(const std::filesystem::path& path, const std::filesystem::path& projectDirectory)
{
    if (path.empty())
    {
        return {};
    }

    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, projectDirectory, ec);
    if (!ec && !relative.empty())
    {
        const std::wstring native = relative.native();
        if (native.rfind(L"..", 0) != 0)
        {
            return PathToUtf8(relative);
        }
    }
    return PathToUtf8(path);
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open file: " + PathToUtf8(path));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error("Failed to write file: " + PathToUtf8(path));
    }
    file << text;
}

bool PathExists(const std::filesystem::path& path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

std::filesystem::path DiscoverRootDirectory()
{
    std::error_code ec;
    std::filesystem::path current = std::filesystem::current_path(ec);
    if (!ec && PathExists(current / "Shaders") && PathExists(current / "Source"))
    {
        return current;
    }

    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::filesystem::path probe = std::filesystem::path(modulePath).parent_path();
    for (int i = 0; i < 6 && !probe.empty(); ++i)
    {
        if (PathExists(probe / "Shaders") && PathExists(probe / "Source"))
        {
            return probe;
        }
        probe = probe.parent_path();
    }
    return current.empty() ? std::filesystem::path(L".") : current;
}

const char* BackgroundModeName(rb::LookDevBackgroundMode mode)
{
    switch (mode)
    {
    case rb::LookDevBackgroundMode::Hdri: return "HDRI";
    case rb::LookDevBackgroundMode::TransparentChecker: return "Checker";
    case rb::LookDevBackgroundMode::SkyColor:
    default: return "Sky Color";
    }
}

rb::LookDevBackgroundMode BackgroundModeFromString(const std::string& text, rb::LookDevBackgroundMode fallback = rb::LookDevBackgroundMode::SkyColor)
{
    if (text == "HDRI" || text == "Hdri" || text == "hdri")
    {
        return rb::LookDevBackgroundMode::Hdri;
    }
    if (text == "Checker" || text == "TransparentChecker" || text == "checker")
    {
        return rb::LookDevBackgroundMode::TransparentChecker;
    }
    if (text == "Sky Color" || text == "SkyColor" || text == "skyColor")
    {
        return rb::LookDevBackgroundMode::SkyColor;
    }
    return fallback;
}

const char* ToneMapperName(rb::ToneMapper mapper)
{
    switch (mapper)
    {
    case rb::ToneMapper::None: return "None";
    case rb::ToneMapper::Reinhard: return "Reinhard";
    case rb::ToneMapper::Aces:
    default: return "ACES";
    }
}

rb::ToneMapper ToneMapperFromString(const std::string& text, rb::ToneMapper fallback = rb::ToneMapper::Aces)
{
    if (text == "None" || text == "none")
    {
        return rb::ToneMapper::None;
    }
    if (text == "Reinhard" || text == "reinhard")
    {
        return rb::ToneMapper::Reinhard;
    }
    if (text == "ACES" || text == "Aces" || text == "aces")
    {
        return rb::ToneMapper::Aces;
    }
    return fallback;
}

const char* DisplayModeName(rb::LookDevDisplayMode mode)
{
    switch (mode)
    {
    case rb::LookDevDisplayMode::BaseColor: return "Base Color";
    case rb::LookDevDisplayMode::Normal: return "Normal";
    case rb::LookDevDisplayMode::Roughness: return "Roughness";
    case rb::LookDevDisplayMode::Metallic: return "Metallic";
    case rb::LookDevDisplayMode::AmbientOcclusion: return "Ambient Occlusion";
    case rb::LookDevDisplayMode::Emissive: return "Emissive";
    case rb::LookDevDisplayMode::LightingOnly: return "Lighting Only";
    case rb::LookDevDisplayMode::ShadowMask: return "Shadow Mask";
    case rb::LookDevDisplayMode::Beauty:
    default: return "Beauty";
    }
}

rb::LookDevDisplayMode DisplayModeFromString(const std::string& text, rb::LookDevDisplayMode fallback = rb::LookDevDisplayMode::Beauty)
{
    if (text == "Beauty" || text == "beauty") return rb::LookDevDisplayMode::Beauty;
    if (text == "Base Color" || text == "BaseColor" || text == "baseColor") return rb::LookDevDisplayMode::BaseColor;
    if (text == "Normal" || text == "normal") return rb::LookDevDisplayMode::Normal;
    if (text == "Roughness" || text == "roughness") return rb::LookDevDisplayMode::Roughness;
    if (text == "Metallic" || text == "metallic") return rb::LookDevDisplayMode::Metallic;
    if (text == "Ambient Occlusion" || text == "AmbientOcclusion" || text == "ambientOcclusion") return rb::LookDevDisplayMode::AmbientOcclusion;
    if (text == "Emissive" || text == "emissive") return rb::LookDevDisplayMode::Emissive;
    if (text == "Lighting Only" || text == "LightingOnly" || text == "lightingOnly") return rb::LookDevDisplayMode::LightingOnly;
    if (text == "Shadow Mask" || text == "ShadowMask" || text == "shadowMask") return rb::LookDevDisplayMode::ShadowMask;
    return fallback;
}

const char* AlphaModeName(rb::AlphaMode mode)
{
    switch (mode)
    {
    case rb::AlphaMode::Mask: return "Mask";
    case rb::AlphaMode::Blend: return "Blend";
    case rb::AlphaMode::Opaque:
    default: return "Opaque";
    }
}

rb::AlphaMode AlphaModeFromString(const std::string& text, rb::AlphaMode fallback = rb::AlphaMode::Opaque)
{
    if (text == "Opaque" || text == "opaque") return rb::AlphaMode::Opaque;
    if (text == "Mask" || text == "mask") return rb::AlphaMode::Mask;
    if (text == "Blend" || text == "blend") return rb::AlphaMode::Blend;
    return fallback;
}

bool ReadOptionalNumber(const cld::JsonValue& value, const char* name, float minValue, float maxValue, float& target, std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(value, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Number || member->number < minValue || member->number > maxValue)
    {
        diagnostics = std::string(name) + " must be a number in range.";
        return false;
    }
    target = static_cast<float>(member->number);
    return true;
}

bool ReadOptionalBool(const cld::JsonValue& value, const char* name, bool& target, std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(value, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Bool)
    {
        diagnostics = std::string(name) + " must be a boolean.";
        return false;
    }
    target = member->boolean;
    return true;
}

bool ReadOptionalFloat3(const cld::JsonValue& value, const char* name, float minValue, float maxValue, std::array<float, 3>& target, std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(value, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Array || member->array.size() != 3)
    {
        diagnostics = std::string(name) + " must be a float3 array.";
        return false;
    }
    std::array<float, 3> updated = target;
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (member->array[i].type != cld::JsonValue::Type::Number || member->array[i].number < minValue || member->array[i].number > maxValue)
        {
            diagnostics = std::string(name) + " contains a value outside the allowed range.";
            return false;
        }
        updated[i] = static_cast<float>(member->array[i].number);
    }
    target = updated;
    return true;
}

bool ReadOptionalFloat4(const cld::JsonValue& value, const char* name, float minValue, float maxValue, std::array<float, 4>& target, std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(value, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Array || member->array.size() != 4)
    {
        diagnostics = std::string(name) + " must be a float4 array.";
        return false;
    }
    std::array<float, 4> updated = target;
    for (std::size_t i = 0; i < 4; ++i)
    {
        if (member->array[i].type != cld::JsonValue::Type::Number || member->array[i].number < minValue || member->array[i].number > maxValue)
        {
            diagnostics = std::string(name) + " contains a value outside the allowed range.";
            return false;
        }
        updated[i] = static_cast<float>(member->array[i].number);
    }
    target = updated;
    return true;
}

std::string Float3Json(const std::array<float, 3>& value)
{
    std::ostringstream json;
    json << "[" << value[0] << "," << value[1] << "," << value[2] << "]";
    return json.str();
}

std::string Float4Json(const std::array<float, 4>& value)
{
    std::ostringstream json;
    json << "[" << value[0] << "," << value[1] << "," << value[2] << "," << value[3] << "]";
    return json.str();
}

void NormalizeDirection(std::array<float, 3>& direction)
{
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length > 0.0001f)
    {
        direction[0] /= length;
        direction[1] /= length;
        direction[2] /= length;
    }
}

bool ExtractFirstJsonObject(const std::string& text, std::string& json)
{
    for (std::size_t start = 0; start < text.size(); ++start)
    {
        if (text[start] != '{')
        {
            continue;
        }

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (std::size_t i = start; i < text.size(); ++i)
        {
            const char ch = text[i];
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (ch == '\\')
                {
                    escaped = true;
                }
                else if (ch == '"')
                {
                    inString = false;
                }
                continue;
            }

            if (ch == '"')
            {
                inString = true;
            }
            else if (ch == '{')
            {
                ++depth;
            }
            else if (ch == '}')
            {
                --depth;
                if (depth == 0)
                {
                    json = text.substr(start, i - start + 1);
                    return true;
                }
                if (depth < 0)
                {
                    break;
                }
            }
        }
    }
    return false;
}

ImFont* ChatTextFont()
{
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts && io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : ImGui::GetFont();
}

void AddScaled(std::array<float, 3>& value, const std::array<float, 3>& direction, float scale)
{
    value[0] += direction[0] * scale;
    value[1] += direction[1] * scale;
    value[2] += direction[2] * scale;
}
}

namespace cld
{
ChatLookDevApp::~ChatLookDevApp()
{
    Shutdown();
}

int ChatLookDevApp::Run(HINSTANCE instance, int showCommand)
{
    try
    {
        Initialize(instance, showCommand);
        MainLoop();
        Shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        MessageBoxA(nullptr, ex.what(), "ChatLookDev startup failed", MB_ICONERROR | MB_OK);
        Shutdown();
        return 1;
    }
}

void ChatLookDevApp::Initialize(HINSTANCE instance, int showCommand)
{
    m_instance = instance;
    m_rootDirectory = DiscoverRootDirectory();
    InitializeDefaults();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = ChatLookDevApp::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"ChatLookDevWindow";
    RegisterClassExW(&windowClass);

    m_hwnd = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"ChatLookDev",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1600,
        980,
        nullptr,
        nullptr,
        instance,
        this);
    if (!m_hwnd)
    {
        throw std::runtime_error("CreateWindowExW failed.");
    }

    ShowWindow(m_hwnd, showCommand);
    UpdateWindow(m_hwnd);
    m_backend.Initialize(m_hwnd, 1600, 980, m_rootDirectory);
    ApplyLookDevSettings();
    ApplyMaterialAssignments();

    m_lastFrameTime = std::chrono::steady_clock::now();
    m_running = true;
    m_initialized = true;
}

void ChatLookDevApp::Shutdown()
{
    if (!m_initialized && !m_hwnd)
    {
        return;
    }
    m_llm.Stop();
    m_backend.Shutdown();
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(L"ChatLookDevWindow", m_instance);
    m_initialized = false;
}

void ChatLookDevApp::InitializeDefaults()
{
    m_project.scenePath.clear();
    m_project.modelTransform = {};
    m_project.skyTopColor = { 0.12f, 0.20f, 0.32f, 1.0f };
    m_project.skyHorizonColor = { 0.03f, 0.04f, 0.055f, 1.0f };
    m_project.lookDevEnvironment.sunIntensity = 10000.0f;
    m_project.lookDevEnvironment.sunDirection = { -0.35f, -0.75f, 0.55f };
    m_project.lookDevEnvironment.sunColor = { 1.0f, 0.96f, 0.88f };
    m_project.lookDevEnvironment.intensity = 1.0f;
    m_project.lookDevEnvironment.backgroundMode = rb::LookDevBackgroundMode::SkyColor;
    m_project.lookDevViewSettings.exposure = 0.0f;
    m_project.lookDevViewSettings.gamma = 2.2f;
    m_project.lookDevViewSettings.toneMapper = rb::ToneMapper::Aces;
    m_project.lookDevViewSettings.displayMode = rb::LookDevDisplayMode::Beauty;
    m_project.materialAssignments.clear();
    m_project.materialAssignments.push_back(rb::MaterialAssignment{ "Default Material", "LookDev PBR" });

    m_llmConfig.modelPath = m_rootDirectory / "Assets" / "Models" / "gemma-4-E4B-it" / "gemma-4-E4B-it-Q4_K_M.gguf";
    m_llmConfig.contextTokens = 4096;
    m_llmConfig.maxTokens = 512;
    m_llmConfig.gpuLayers = 0;
    m_llmConfig.threads = 1;
    m_llmConfig.temperature = 0.2f;
    m_llmConfig.topP = 0.9f;
    m_llmConfig.topK = 40;
}

void ChatLookDevApp::MainLoop()
{
    MSG message = {};
    while (m_running)
    {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
            {
                m_running = false;
            }
        }
        if (!m_running)
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;

        DrainLlmEvents();
        BeginFrame();
        DrawUi();
        RenderFrame(deltaSeconds);
    }
}

void ChatLookDevApp::BeginFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ChatLookDevApp::DrawUi()
{
    DrawDockSpace();
    DrawViewportPanel();
    DrawScenePanel();
    DrawMaterialPanel();
    DrawLightingPanel();
    DrawAiChatPanel();
    DrawDiagnosticsPanel();
}

void ChatLookDevApp::DrawDockSpace()
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("ChatLookDev DockSpace", nullptr, flags);
    ImGui::PopStyleVar(2);

    DrawMainMenu();
    ImGuiID dockspaceId = ImGui::GetID("ChatLookDevDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

void ChatLookDevApp::DrawMainMenu()
{
    if (!ImGui::BeginMenuBar())
    {
        return;
    }
    if (ImGui::BeginMenu("Project"))
    {
        if (ImGui::MenuItem("New"))
        {
            InitializeDefaults();
            UsePreviewScene();
            ApplyLookDevSettings();
            m_projectDirty = false;
        }
        if (ImGui::MenuItem("Open..."))
        {
            LoadProject();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S"))
        {
            SaveProject();
        }
        if (ImGui::MenuItem("Save As..."))
        {
            SaveProjectAs();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Scene"))
    {
        if (ImGui::MenuItem("Import glTF / GLB..."))
        {
            const auto path = OpenFileDialog(L"glTF Model\0*.gltf;*.glb\0All Files\0*.*\0");
            if (!path.empty())
            {
                LoadScenePath(path);
            }
        }
        if (ImGui::MenuItem("Use Preview Cube"))
        {
            UsePreviewScene();
        }
        if (ImGui::MenuItem("Reset Camera"))
        {
            m_backend.ResetCameraToScene();
            m_project.hasViewportCamera = false;
            MarkProjectDirty();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Lighting"))
    {
        if (ImGui::MenuItem("Load HDRI..."))
        {
            const auto path = OpenFileDialog(L"HDR Image\0*.hdr;*.dds\0All Files\0*.*\0");
            if (!path.empty())
            {
                LoadEnvironmentPath(path);
            }
        }
        if (ImGui::MenuItem("Clear HDRI"))
        {
            LoadEnvironmentPath({});
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void ChatLookDevApp::DrawViewportPanel()
{
    ImGui::Begin("Viewport");
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float width = std::max(1.0f, available.x);
    const float height = std::max(1.0f, available.y);
    m_backend.ResizeSceneTarget(static_cast<UINT>(width), static_cast<UINT>(height));

    const ImVec2 imagePos = ImGui::GetCursorScreenPos();
    ImTextureID textureId = static_cast<ImTextureID>(m_backend.SceneSrvGpu().ptr);
    ImGui::Image(textureId, ImVec2(width, height));
    HandleViewportInput(imagePos, ImVec2(imagePos.x + width, imagePos.y + height));
    ImGui::End();
}

void ChatLookDevApp::HandleViewportInput(const ImVec2& imageMin, const ImVec2& imageMax)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool hovered = mouse.x >= imageMin.x && mouse.y >= imageMin.y && mouse.x <= imageMax.x && mouse.y <= imageMax.y;
    if (!hovered || io.WantCaptureKeyboard)
    {
        return;
    }

    const rb::ViewportCamera camera = m_backend.CameraState();
    const float moveScale = std::max(camera.distance, 0.5f) * io.DeltaTime * (io.KeyShift ? 2.0f : 0.65f) * (io.KeyCtrl ? 0.25f : 1.0f);
    const std::array<float, 3> cameraRight = { std::cos(camera.yaw), 0.0f, -std::sin(camera.yaw) };
    const std::array<float, 3> cameraForward = { -std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw) };
    const std::array<float, 3> worldUp = { 0.0f, 1.0f, 0.0f };

    bool modelChanged = false;
    if (ImGui::IsKeyDown(ImGuiKey_W))
    {
        AddScaled(m_project.modelTransform.translation, cameraForward, moveScale);
        modelChanged = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_S))
    {
        AddScaled(m_project.modelTransform.translation, cameraForward, -moveScale);
        modelChanged = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_D))
    {
        AddScaled(m_project.modelTransform.translation, cameraRight, moveScale);
        modelChanged = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_A))
    {
        AddScaled(m_project.modelTransform.translation, cameraRight, -moveScale);
        modelChanged = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_E))
    {
        AddScaled(m_project.modelTransform.translation, worldUp, moveScale);
        modelChanged = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_Q))
    {
        AddScaled(m_project.modelTransform.translation, worldUp, -moveScale);
        modelChanged = true;
    }

    const bool modelMouseMode = io.KeyShift;
    const ImVec2 delta = io.MouseDelta;
    if (modelMouseMode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        m_project.modelTransform.rotationDegrees[1] += delta.x * 0.35f;
        m_project.modelTransform.rotationDegrees[0] += delta.y * 0.35f;
        modelChanged = true;
    }
    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        m_backend.OrbitCamera(delta.x * 0.006f, delta.y * 0.006f);
        m_project.viewportCamera = m_backend.CameraState();
        m_project.hasViewportCamera = true;
        MarkProjectDirty();
    }
    if (modelMouseMode && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
    {
        const float dragScale = std::max(camera.distance, 0.5f) * 0.0015f;
        AddScaled(m_project.modelTransform.translation, cameraRight, delta.x * dragScale);
        AddScaled(m_project.modelTransform.translation, worldUp, -delta.y * dragScale);
        modelChanged = true;
    }
    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
    {
        m_backend.PanCamera(delta.x * 0.002f, delta.y * 0.002f);
        m_project.viewportCamera = m_backend.CameraState();
        m_project.hasViewportCamera = true;
        MarkProjectDirty();
    }
    const float wheel = io.MouseWheel;
    if (std::abs(wheel) > 0.0f)
    {
        if (modelMouseMode)
        {
            AddScaled(m_project.modelTransform.translation, cameraForward, wheel * std::max(camera.distance, 0.5f) * 0.08f);
            modelChanged = true;
        }
        else
        {
            m_backend.DollyCamera(wheel);
            m_project.viewportCamera = m_backend.CameraState();
            m_project.hasViewportCamera = true;
            MarkProjectDirty();
        }
    }

    if (modelChanged)
    {
        ApplyModelTransform();
        MarkProjectDirty();
    }
}

void ChatLookDevApp::DrawScenePanel()
{
    ImGui::Begin("Scene");
    const std::string scenePath = PathToUtf8(m_project.scenePath);
    ImGui::TextUnformatted("Scene");
    ImGui::BeginDisabled();
    ImGui::InputText("##ScenePath", const_cast<char*>(scenePath.c_str()), scenePath.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    if (ImGui::Button("Import glTF / GLB"))
    {
        const auto path = OpenFileDialog(L"glTF Model\0*.gltf;*.glb\0All Files\0*.*\0");
        if (!path.empty())
        {
            LoadScenePath(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Preview Cube"))
    {
        UsePreviewScene();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Camera"))
    {
        m_backend.ResetCameraToScene();
        m_project.viewportCamera = m_backend.CameraState();
        m_project.hasViewportCamera = true;
        MarkProjectDirty();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Model Transform");
    bool transformChanged = false;
    transformChanged |= ImGui::DragFloat3("Translation", m_project.modelTransform.translation.data(), 0.01f, -1000000.0f, 1000000.0f, "%.3f");
    transformChanged |= ImGui::DragFloat3("Rotation deg", m_project.modelTransform.rotationDegrees.data(), 0.25f, -36000.0f, 36000.0f, "%.2f");
    if (transformChanged)
    {
        ApplyModelTransform();
        MarkProjectDirty();
    }
    if (ImGui::Button("Reset Transform"))
    {
        m_project.modelTransform = {};
        ApplyModelTransform();
        MarkProjectDirty();
    }

    const RendererStats stats = m_backend.Stats();
    ImGui::Separator();
    ImGui::Text("Vertices: %u", stats.vertexCount);
    ImGui::Text("Indices: %u", stats.indexCount);
    ImGui::Text("Materials: %zu", m_project.materialAssignments.size());
    ImGui::Separator();
    ImGui::TextWrapped("%s", m_sceneDiagnostics.c_str());
    ImGui::End();
}

void ChatLookDevApp::DrawMaterialPanel()
{
    ImGui::Begin("Material");
    EnsureMaterialSelection();
    if (m_project.materialAssignments.empty())
    {
        ImGui::TextUnformatted("No material.");
        ImGui::End();
        return;
    }

    std::vector<const char*> names;
    names.reserve(m_project.materialAssignments.size());
    for (const rb::MaterialAssignment& material : m_project.materialAssignments)
    {
        names.push_back(material.materialName.c_str());
    }
    int selected = static_cast<int>(m_selectedMaterial);
    if (ImGui::Combo("Material", &selected, names.data(), static_cast<int>(names.size())))
    {
        m_selectedMaterial = static_cast<std::size_t>(std::max(selected, 0));
    }

    rb::MaterialAssignment& material = m_project.materialAssignments[m_selectedMaterial];
    bool changed = false;
    changed |= ImGui::ColorEdit4("Base Color", material.baseColorFactor.data());
    changed |= ImGui::SliderFloat("Roughness", &material.roughnessFactor, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Metallic", &material.metallicFactor, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Normal Strength", &material.normalStrength, 0.0f, 2.0f);
    changed |= ImGui::SliderFloat("Occlusion", &material.occlusionStrength, 0.0f, 1.0f);
    changed |= ImGui::ColorEdit4("Emissive", material.emissiveFactor.data(), ImGuiColorEditFlags_Float);

    const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
    int alphaIndex = static_cast<int>(material.alphaMode);
    if (ImGui::Combo("Alpha Mode", &alphaIndex, alphaModes, IM_ARRAYSIZE(alphaModes)))
    {
        material.alphaMode = static_cast<rb::AlphaMode>(std::clamp(alphaIndex, 0, 2));
        changed = true;
    }
    if (material.alphaMode == rb::AlphaMode::Mask)
    {
        changed |= ImGui::SliderFloat("Alpha Cutoff", &material.alphaCutoff, 0.0f, 1.0f);
    }
    changed |= ImGui::Checkbox("Packed ORM", &material.packedOcclusionRoughnessMetallic);
    changed |= ImGui::Checkbox("Flip Normal Green", &material.flipNormalGreen);

    if (changed)
    {
        ApplyMaterialAssignments();
        MarkProjectDirty();
    }

    ImGui::SeparatorText("Textures");
    for (std::size_t slot = 0; slot < TextureSlotCount; ++slot)
    {
        ImGui::PushID(static_cast<int>(slot));
        bool overrideEnabled = material.textureOverrideEnabled[slot];
        if (ImGui::Checkbox(TextureSlotLabels[slot], &overrideEnabled))
        {
            material.textureOverrideEnabled[slot] = overrideEnabled;
            ApplyMaterialAssignments();
            MarkProjectDirty();
        }
        const std::string pathText = PathToUtf8(material.textureOverrides[slot]);
        ImGui::BeginDisabled();
        ImGui::InputText("##TexturePath", const_cast<char*>(pathText.c_str()), pathText.size() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Browse"))
        {
            const auto path = OpenFileDialog(L"Texture\0*.png;*.jpg;*.jpeg;*.tga;*.dds;*.hdr\0All Files\0*.*\0");
            if (!path.empty())
            {
                material.textureOverrideEnabled[slot] = true;
                material.textureOverrides[slot] = path.wstring();
                ApplyMaterialAssignments();
                MarkProjectDirty();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            material.textureOverrideEnabled[slot] = false;
            material.textureOverrides[slot].clear();
            ApplyMaterialAssignments();
            MarkProjectDirty();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void ChatLookDevApp::DrawLightingPanel()
{
    ImGui::Begin("Lighting");
    rb::LookDevEnvironment& environment = m_project.lookDevEnvironment;
    rb::LookDevViewSettings& view = m_project.lookDevViewSettings;
    bool changed = false;

    const std::string hdriPath = PathToUtf8(environment.environmentPath);
    ImGui::TextUnformatted("HDRI");
    ImGui::BeginDisabled();
    ImGui::InputText("##HdriPath", const_cast<char*>(hdriPath.c_str()), hdriPath.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    if (ImGui::Button("Load HDRI"))
    {
        const auto path = OpenFileDialog(L"HDR Image\0*.hdr;*.dds\0All Files\0*.*\0");
        if (!path.empty())
        {
            LoadEnvironmentPath(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        LoadEnvironmentPath({});
    }

    const char* backgrounds[] = { "Sky Color", "HDRI", "Checker" };
    int backgroundIndex = static_cast<int>(environment.backgroundMode);
    if (ImGui::Combo("Background", &backgroundIndex, backgrounds, IM_ARRAYSIZE(backgrounds)))
    {
        environment.backgroundMode = static_cast<rb::LookDevBackgroundMode>(std::clamp(backgroundIndex, 0, 2));
        changed = true;
    }
    changed |= ImGui::SliderFloat("HDRI Intensity", &environment.intensity, 0.0f, 8.0f);
    changed |= ImGui::SliderAngle("HDRI Yaw", &environment.rotationYaw, -180.0f, 180.0f);

    ImGui::SeparatorText("Sun");
    changed |= ImGui::DragFloat3("Direction", environment.sunDirection.data(), 0.01f, -1.0f, 1.0f);
    if (ImGui::Button("Normalize Direction"))
    {
        NormalizeDirection(environment.sunDirection);
        changed = true;
    }
    changed |= ImGui::ColorEdit3("Color", environment.sunColor.data());
    changed |= ImGui::DragFloat("Illuminance (lux)", &environment.sunIntensity, 100.0f, 0.0f, 200000.0f, "%.0f");

    ImGui::SeparatorText("View");
    changed |= ImGui::SliderFloat("Exposure EV", &view.exposure, -8.0f, 8.0f);
    changed |= ImGui::SliderFloat("Gamma", &view.gamma, 0.8f, 3.0f);
    const char* toneMappers[] = { "None", "Reinhard", "ACES" };
    int toneIndex = static_cast<int>(view.toneMapper);
    if (ImGui::Combo("Tone Mapper", &toneIndex, toneMappers, IM_ARRAYSIZE(toneMappers)))
    {
        view.toneMapper = static_cast<rb::ToneMapper>(std::clamp(toneIndex, 0, 2));
        changed = true;
    }
    const char* displayModes[] =
    {
        "Beauty",
        "Base Color",
        "Normal",
        "Roughness",
        "Metallic",
        "Ambient Occlusion",
        "Emissive",
        "Lighting Only",
        "Shadow Mask",
    };
    int displayIndex = static_cast<int>(view.displayMode);
    if (ImGui::Combo("Display Mode", &displayIndex, displayModes, IM_ARRAYSIZE(displayModes)))
    {
        view.displayMode = static_cast<rb::LookDevDisplayMode>(std::clamp(displayIndex, 0, 8));
        changed = true;
    }
    changed |= ImGui::Checkbox("Turntable", &view.turntableEnabled);
    changed |= ImGui::SliderFloat("Turntable Speed", &view.turntableSpeed, -3.0f, 3.0f);

    ImGui::SeparatorText("Sky Color");
    changed |= ImGui::ColorEdit4("Top", m_project.skyTopColor.data());
    changed |= ImGui::ColorEdit4("Horizon", m_project.skyHorizonColor.data());

    if (changed)
    {
        ApplyLookDevSettings();
        MarkProjectDirty();
    }
    ImGui::End();
}

void ChatLookDevApp::DrawAiChatPanel()
{
    ImGui::Begin("AI Chat");
    const LocalLlmStatus status = m_llm.Status();
    ImGui::Text("State: %s", status.stateText.c_str());
    if (!status.lastError.empty())
    {
        ImGui::TextWrapped("Error: %s", status.lastError.c_str());
    }

    const std::string modelPath = PathToUtf8(m_llmConfig.modelPath);
    ImGui::BeginDisabled();
    ImGui::InputText("Model", const_cast<char*>(modelPath.c_str()), modelPath.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    if (ImGui::Button("Browse Model"))
    {
        const auto path = OpenFileDialog(L"GGUF Model\0*.gguf\0All Files\0*.*\0");
        if (!path.empty())
        {
            m_llmConfig.modelPath = path;
            MarkProjectDirty();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Model"))
    {
        LoadLocalModel();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        m_llm.Stop();
    }
    if (!PathExists(m_llmConfig.modelPath))
    {
        ImGui::TextWrapped("Default GGUF is not present. Place gemma-4-E4B-it-Q4_K_M.gguf under Assets/Models/gemma-4-E4B-it or browse to another GGUF.");
    }

    ImGui::SliderInt("Context Tokens", &m_llmConfig.contextTokens, 1024, 32768);
    ImGui::SliderInt("Max Reply Tokens", &m_llmConfig.maxTokens, 64, 2048);
    ImGui::SliderInt("CPU Threads", &m_llmConfig.threads, 0, 64);
    ImGui::SliderInt("GPU Layers", &m_llmConfig.gpuLayers, 0, 128);
    ImGui::SliderFloat("Temperature", &m_llmConfig.temperature, 0.0f, 1.5f);
    ImGui::SliderFloat("Top P", &m_llmConfig.topP, 0.05f, 1.0f);

    ImGui::Separator();
    const float transcriptHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y - 150.0f);
    ImGui::BeginChild("Transcript", ImVec2(0.0f, transcriptHeight), ImGuiChildFlags_Borders);
    ImGui::PushFont(ChatTextFont());
    for (const ChatMessage& message : m_chatMessages)
    {
        ImGui::TextColored(message.role == "assistant" ? ImVec4(0.45f, 0.75f, 1.0f, 1.0f) : ImVec4(0.95f, 0.95f, 0.95f, 1.0f), "%s", message.role.c_str());
        ImGui::TextWrapped("%s", message.text.c_str());
        ImGui::Spacing();
    }
    if (m_scrollChatToBottom)
    {
        ImGui::SetScrollHereY(1.0f);
        m_scrollChatToBottom = false;
    }
    ImGui::PopFont();
    ImGui::EndChild();

    ImGui::PushFont(ChatTextFont());
    ImGui::InputTextMultiline("##ChatInput", m_chatInput.data(), m_chatInput.size(), ImVec2(-1.0f, 80.0f));
    ImGui::PopFont();
    const bool canSend = status.state == LocalLlmState::Ready && TrimAscii(m_chatInput.data()).size() > 0;
    ImGui::BeginDisabled(!canSend);
    if (ImGui::Button("Send"))
    {
        SendChatPrompt();
    }
    ImGui::EndDisabled();
    if (!m_lastActionDiagnostics.empty())
    {
        ImGui::TextWrapped("%s", m_lastActionDiagnostics.c_str());
    }
    ImGui::End();
}

void ChatLookDevApp::DrawDiagnosticsPanel()
{
    ImGui::Begin("Diagnostics / Stats");
    const RendererStats stats = m_backend.Stats();
    ImGui::Text("Adapter: %s", stats.adapterName.c_str());
    ImGui::Text("Shader Model 6.9: %s", stats.shaderModel69 ? "yes" : "no");
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(stats.frameNumber));
    ImGui::Text("Frame Time: %.3f ms", stats.lastFrameMs);
    ImGui::Text("Viewport: %ux%u", stats.sceneWidth, stats.sceneHeight);
    ImGui::TextWrapped("Environment: %s", stats.environmentStatus.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("Root: %s", PathToUtf8(m_rootDirectory).c_str());
    if (!m_project.path.empty())
    {
        ImGui::TextWrapped("Project: %s%s", PathToUtf8(m_project.path).c_str(), m_projectDirty ? " *" : "");
    }
    if (!m_projectDiagnostics.empty())
    {
        ImGui::TextWrapped("%s", m_projectDiagnostics.c_str());
    }
    if (!m_lastActionDiagnostics.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("AI: %s", m_lastActionDiagnostics.c_str());
    }
    ImGui::End();
}

void ChatLookDevApp::RenderFrame(float deltaSeconds)
{
    m_backend.Render(deltaSeconds);
}

std::filesystem::path ChatLookDevApp::OpenFileDialog(const wchar_t* filter) const
{
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW openFile = {};
    openFile.lStructSize = sizeof(openFile);
    openFile.hwndOwner = m_hwnd;
    openFile.lpstrFilter = filter;
    openFile.lpstrFile = fileName;
    openFile.nMaxFile = MAX_PATH;
    openFile.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&openFile))
    {
        return std::filesystem::path(fileName);
    }
    return {};
}

std::filesystem::path ChatLookDevApp::SaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, const wchar_t* defaultName) const
{
    wchar_t fileName[MAX_PATH] = {};
    if (!m_project.path.empty())
    {
        wcsncpy_s(fileName, std::filesystem::path(m_project.path).filename().wstring().c_str(), _TRUNCATE);
    }
    else
    {
        wcsncpy_s(fileName, defaultName, _TRUNCATE);
    }

    OPENFILENAMEW saveFile = {};
    saveFile.lStructSize = sizeof(saveFile);
    saveFile.hwndOwner = m_hwnd;
    saveFile.lpstrFilter = filter;
    saveFile.lpstrFile = fileName;
    saveFile.nMaxFile = MAX_PATH;
    saveFile.lpstrDefExt = defaultExtension;
    saveFile.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&saveFile))
    {
        return std::filesystem::path(fileName);
    }
    return {};
}

bool ChatLookDevApp::LoadScenePath(const std::filesystem::path& path)
{
    rb::SceneImportResult result = m_sceneImporter.ImportScene(path.wstring());
    if (!result.succeeded)
    {
        m_sceneDiagnostics = "Scene import failed: " + result.diagnostics;
        return false;
    }

    ApplyImportedTextureOverrides(result.scene);
    std::string diagnostics;
    if (!m_backend.LoadSceneMesh(result.scene, diagnostics))
    {
        m_sceneDiagnostics = "Renderer scene load failed: " + diagnostics;
        return false;
    }

    m_importedScene = std::move(result.scene);
    m_project.scenePath = AbsoluteLexicalPath(path).wstring();
    m_project.modelTransform = {};
    ApplyModelTransform();
    m_project.materialAssignments.clear();
    for (const rb::SceneMaterial& material : m_importedScene.materials)
    {
        m_project.materialAssignments.push_back(material.assignment);
    }
    if (m_project.materialAssignments.empty())
    {
        m_project.materialAssignments.push_back(rb::MaterialAssignment{ "Default Material", "LookDev PBR" });
    }
    m_selectedMaterial = 0;
    ApplyMaterialAssignments();
    m_sceneDiagnostics = result.diagnostics + "\n" + diagnostics;
    MarkProjectDirty();
    return true;
}

void ChatLookDevApp::ApplyImportedTextureOverrides(rb::ImportedScene& scene)
{
    for (rb::SceneMaterial& material : scene.materials)
    {
        std::array<std::wstring, TextureSlotCount> paths =
        {
            material.baseColorTexturePath,
            material.normalTexturePath,
            material.roughnessTexturePath,
            material.metallicTexturePath,
            material.occlusionTexturePath,
            material.emissiveTexturePath,
        };
        for (std::size_t slot = 0; slot < TextureSlotCount; ++slot)
        {
            if (!paths[slot].empty())
            {
                material.assignment.textureOverrideEnabled[slot] = true;
                material.assignment.textureOverrides[slot] = paths[slot];
            }
        }
    }
}

bool ChatLookDevApp::LoadEnvironmentPath(const std::filesystem::path& path)
{
    std::string diagnostics;
    if (!m_backend.UpdateEnvironmentTexture(path.wstring(), diagnostics))
    {
        m_sceneDiagnostics = "HDRI load failed: " + diagnostics;
        return false;
    }
    m_project.lookDevEnvironment.environmentPath = path.wstring();
    m_project.lookDevEnvironment.backgroundMode = path.empty() ? rb::LookDevBackgroundMode::SkyColor : rb::LookDevBackgroundMode::Hdri;
    ApplyLookDevSettings();
    m_sceneDiagnostics = diagnostics;
    MarkProjectDirty();
    return true;
}

void ChatLookDevApp::UsePreviewScene()
{
    m_backend.ResetPreviewScene();
    m_importedScene = {};
    m_project.scenePath.clear();
    m_project.modelTransform = {};
    ApplyModelTransform();
    m_project.materialAssignments.clear();
    m_project.materialAssignments.push_back(rb::MaterialAssignment{ "Default Material", "LookDev PBR" });
    m_selectedMaterial = 0;
    ApplyMaterialAssignments();
    m_sceneDiagnostics = "Using built-in preview cube.";
    MarkProjectDirty();
}

void ChatLookDevApp::ApplyModelTransform()
{
    m_backend.SetModelTransform(m_project.modelTransform);
}

void ChatLookDevApp::ApplyLookDevSettings()
{
    NormalizeDirection(m_project.lookDevEnvironment.sunDirection);
    m_backend.SetSkyColors(m_project.skyTopColor, m_project.skyHorizonColor);
    m_backend.SetLookDevEnvironment(m_project.lookDevEnvironment);
    m_backend.SetLookDevViewSettings(m_project.lookDevViewSettings);
}

void ChatLookDevApp::ApplyMaterialAssignments()
{
    if (m_project.materialAssignments.empty())
    {
        m_project.materialAssignments.push_back(rb::MaterialAssignment{ "Default Material", "LookDev PBR" });
    }
    m_backend.SetMaterialAssignments(m_project.materialAssignments);
    EnsureMaterialSelection();
}

void ChatLookDevApp::EnsureMaterialSelection()
{
    if (m_project.materialAssignments.empty())
    {
        m_selectedMaterial = 0;
        return;
    }
    if (m_selectedMaterial >= m_project.materialAssignments.size())
    {
        m_selectedMaterial = m_project.materialAssignments.size() - 1;
    }
}

void ChatLookDevApp::MarkProjectDirty()
{
    m_projectDirty = true;
}

void ChatLookDevApp::SaveProject()
{
    if (m_project.path.empty())
    {
        SaveProjectAs();
        return;
    }
    SaveProjectToDisk(m_project.path);
}

void ChatLookDevApp::SaveProjectAs()
{
    const auto path = SaveFileDialog(L"ChatLookDev Project\0*.chatlookdev.json;*.json\0All Files\0*.*\0", L"chatlookdev.json", L"Untitled.chatlookdev.json");
    if (!path.empty())
    {
        SaveProjectToDisk(path);
    }
}

bool ChatLookDevApp::SaveProjectToDisk(const std::filesystem::path& requestedPath)
{
    try
    {
        std::filesystem::path path = requestedPath;
        if (path.extension().empty())
        {
            path += L".chatlookdev.json";
        }
        path = AbsoluteLexicalPath(path);
        const std::filesystem::path projectDirectory = path.parent_path();
        m_project.viewportCamera = m_backend.CameraState();
        m_project.hasViewportCamera = true;

        std::ostringstream json;
        json << "{\n";
        json << "  \"version\": 1,\n";
        json << "  \"renderer\": \"D3D12\",\n";
        json << "  \"scenePath\": \"" << EscapeJson(ProjectPathString(m_project.scenePath, projectDirectory)) << "\",\n";
        json << "  \"modelTransform\": { "
             << "\"translation\": " << Float3Json(m_project.modelTransform.translation) << ", "
             << "\"rotationDegrees\": " << Float3Json(m_project.modelTransform.rotationDegrees) << " },\n";
        json << "  \"skyTopColor\": " << Float4Json(m_project.skyTopColor) << ",\n";
        json << "  \"skyHorizonColor\": " << Float4Json(m_project.skyHorizonColor) << ",\n";
        json << "  \"camera\": { "
             << "\"target\": " << Float3Json(m_project.viewportCamera.target) << ", "
             << "\"yaw\": " << m_project.viewportCamera.yaw << ", "
             << "\"pitch\": " << m_project.viewportCamera.pitch << ", "
             << "\"distance\": " << m_project.viewportCamera.distance << " },\n";
        json << "  \"environment\": { "
             << "\"hdriPath\": \"" << EscapeJson(ProjectPathString(m_project.lookDevEnvironment.environmentPath, projectDirectory)) << "\", "
             << "\"rotationYaw\": " << m_project.lookDevEnvironment.rotationYaw << ", "
             << "\"intensity\": " << m_project.lookDevEnvironment.intensity << ", "
             << "\"backgroundMode\": \"" << BackgroundModeName(m_project.lookDevEnvironment.backgroundMode) << "\", "
             << "\"sunDirection\": " << Float3Json(m_project.lookDevEnvironment.sunDirection) << ", "
             << "\"sunColor\": " << Float3Json(m_project.lookDevEnvironment.sunColor) << ", "
             << "\"illuminanceLux\": " << m_project.lookDevEnvironment.sunIntensity << " },\n";
        json << "  \"view\": { "
             << "\"exposure\": " << m_project.lookDevViewSettings.exposure << ", "
             << "\"gamma\": " << m_project.lookDevViewSettings.gamma << ", "
             << "\"toneMapper\": \"" << ToneMapperName(m_project.lookDevViewSettings.toneMapper) << "\", "
             << "\"displayMode\": \"" << DisplayModeName(m_project.lookDevViewSettings.displayMode) << "\", "
             << "\"turntableEnabled\": " << (m_project.lookDevViewSettings.turntableEnabled ? "true" : "false") << ", "
             << "\"turntableSpeed\": " << m_project.lookDevViewSettings.turntableSpeed << " },\n";
        json << "  \"llm\": { "
             << "\"modelPath\": \"" << EscapeJson(ProjectPathString(m_llmConfig.modelPath, projectDirectory)) << "\", "
             << "\"contextTokens\": " << m_llmConfig.contextTokens << ", "
             << "\"maxTokens\": " << m_llmConfig.maxTokens << ", "
             << "\"gpuLayers\": " << m_llmConfig.gpuLayers << ", "
             << "\"threads\": " << m_llmConfig.threads << ", "
             << "\"temperature\": " << m_llmConfig.temperature << ", "
             << "\"topP\": " << m_llmConfig.topP << ", "
             << "\"topK\": " << m_llmConfig.topK << " },\n";
        json << "  \"materials\": [\n";
        for (std::size_t i = 0; i < m_project.materialAssignments.size(); ++i)
        {
            const rb::MaterialAssignment& material = m_project.materialAssignments[i];
            json << "    { "
                 << "\"name\": \"" << EscapeJson(material.materialName) << "\", "
                 << "\"baseColorFactor\": " << Float4Json(material.baseColorFactor) << ", "
                 << "\"emissiveFactor\": " << Float4Json(material.emissiveFactor) << ", "
                 << "\"roughnessFactor\": " << material.roughnessFactor << ", "
                 << "\"metallicFactor\": " << material.metallicFactor << ", "
                 << "\"normalStrength\": " << material.normalStrength << ", "
                 << "\"occlusionStrength\": " << material.occlusionStrength << ", "
                 << "\"alphaMode\": \"" << AlphaModeName(material.alphaMode) << "\", "
                 << "\"alphaCutoff\": " << material.alphaCutoff << ", "
                 << "\"packedOcclusionRoughnessMetallic\": " << (material.packedOcclusionRoughnessMetallic ? "true" : "false") << ", "
                 << "\"flipNormalGreen\": " << (material.flipNormalGreen ? "true" : "false") << ", "
                 << "\"textures\": { ";
            for (std::size_t slot = 0; slot < TextureSlotCount; ++slot)
            {
                if (slot > 0)
                {
                    json << ", ";
                }
                json << "\"" << TextureSlotJsonNames[slot] << "\": { "
                     << "\"override\": " << (material.textureOverrideEnabled[slot] ? "true" : "false") << ", "
                     << "\"path\": \"" << EscapeJson(ProjectPathString(material.textureOverrides[slot], projectDirectory)) << "\" }";
            }
            json << " } }";
            if (i + 1 < m_project.materialAssignments.size())
            {
                json << ",";
            }
            json << "\n";
        }
        json << "  ]\n";
        json << "}\n";

        WriteTextFile(path, json.str());
        m_project.path = path.wstring();
        m_projectDirty = false;
        m_projectDiagnostics = "Project saved.";
        return true;
    }
    catch (const std::exception& ex)
    {
        m_projectDiagnostics = "Project save failed: " + std::string(ex.what());
        return false;
    }
}

void ChatLookDevApp::LoadProject()
{
    const auto path = OpenFileDialog(L"ChatLookDev Project\0*.chatlookdev.json;*.json\0All Files\0*.*\0");
    if (!path.empty())
    {
        LoadProjectFromDisk(path);
    }
}

bool ChatLookDevApp::LoadProjectFromDisk(const std::filesystem::path& requestedPath)
{
    try
    {
        const std::filesystem::path path = AbsoluteLexicalPath(requestedPath);
        const std::filesystem::path projectDirectory = path.parent_path();
        const JsonValue root = JsonParser(TrimAscii(ReadTextFile(path))).Parse();
        if (root.type != JsonValue::Type::Object)
        {
            throw std::runtime_error("Project root must be an object.");
        }

        rb::ProjectFile loadedProject;
        LocalLlmConfig loadedLlm = m_llmConfig;
        loadedProject.path = path.wstring();
        loadedProject.scenePath = ResolveProjectPath(JsonStringOr(root, "scenePath"), projectDirectory).wstring();
        loadedProject.skyTopColor = JsonFloat4Or(root, "skyTopColor", loadedProject.skyTopColor);
        loadedProject.skyHorizonColor = JsonFloat4Or(root, "skyHorizonColor", loadedProject.skyHorizonColor);

        if (const JsonValue* modelTransform = FindMember(root, "modelTransform"); modelTransform && modelTransform->type == JsonValue::Type::Object)
        {
            loadedProject.modelTransform.translation = JsonFloat3Or(*modelTransform, "translation", loadedProject.modelTransform.translation);
            loadedProject.modelTransform.rotationDegrees = JsonFloat3Or(*modelTransform, "rotationDegrees", loadedProject.modelTransform.rotationDegrees);
            if (const JsonValue* rotationRadians = FindMember(*modelTransform, "rotationRadians"); rotationRadians && rotationRadians->type == JsonValue::Type::Array && rotationRadians->array.size() >= 3)
            {
                const std::array<float, 3> zeroRadians = { 0.0f, 0.0f, 0.0f };
                std::array<float, 3> radians = JsonFloat3Or(*modelTransform, "rotationRadians", zeroRadians);
                loadedProject.modelTransform.rotationDegrees =
                {
                    radians[0] * 57.2957795f,
                    radians[1] * 57.2957795f,
                    radians[2] * 57.2957795f,
                };
            }
        }

        if (const JsonValue* camera = FindMember(root, "camera"); camera && camera->type == JsonValue::Type::Object)
        {
            loadedProject.viewportCamera.target = JsonFloat3Or(*camera, "target", loadedProject.viewportCamera.target);
            loadedProject.viewportCamera.yaw = static_cast<float>(JsonNumberOr(*camera, "yaw", loadedProject.viewportCamera.yaw));
            loadedProject.viewportCamera.pitch = static_cast<float>(JsonNumberOr(*camera, "pitch", loadedProject.viewportCamera.pitch));
            loadedProject.viewportCamera.distance = static_cast<float>(JsonNumberOr(*camera, "distance", loadedProject.viewportCamera.distance));
            loadedProject.hasViewportCamera = true;
        }

        if (const JsonValue* environment = FindMember(root, "environment"); environment && environment->type == JsonValue::Type::Object)
        {
            loadedProject.lookDevEnvironment.environmentPath = ResolveProjectPath(JsonStringOr(*environment, "hdriPath"), projectDirectory).wstring();
            loadedProject.lookDevEnvironment.rotationYaw = static_cast<float>(JsonNumberOr(*environment, "rotationYaw", loadedProject.lookDevEnvironment.rotationYaw));
            loadedProject.lookDevEnvironment.intensity = static_cast<float>(JsonNumberOr(*environment, "intensity", loadedProject.lookDevEnvironment.intensity));
            loadedProject.lookDevEnvironment.backgroundMode = BackgroundModeFromString(JsonStringOr(*environment, "backgroundMode"), loadedProject.lookDevEnvironment.backgroundMode);
            loadedProject.lookDevEnvironment.sunDirection = JsonFloat3Or(*environment, "sunDirection", loadedProject.lookDevEnvironment.sunDirection);
            loadedProject.lookDevEnvironment.sunColor = JsonFloat3Or(*environment, "sunColor", loadedProject.lookDevEnvironment.sunColor);
            loadedProject.lookDevEnvironment.sunIntensity = static_cast<float>(JsonNumberOr(*environment, "illuminanceLux", loadedProject.lookDevEnvironment.sunIntensity));
        }

        if (const JsonValue* view = FindMember(root, "view"); view && view->type == JsonValue::Type::Object)
        {
            loadedProject.lookDevViewSettings.exposure = static_cast<float>(JsonNumberOr(*view, "exposure", loadedProject.lookDevViewSettings.exposure));
            loadedProject.lookDevViewSettings.gamma = static_cast<float>(JsonNumberOr(*view, "gamma", loadedProject.lookDevViewSettings.gamma));
            loadedProject.lookDevViewSettings.toneMapper = ToneMapperFromString(JsonStringOr(*view, "toneMapper"), loadedProject.lookDevViewSettings.toneMapper);
            loadedProject.lookDevViewSettings.displayMode = DisplayModeFromString(JsonStringOr(*view, "displayMode"), loadedProject.lookDevViewSettings.displayMode);
            loadedProject.lookDevViewSettings.turntableEnabled = JsonBoolOr(*view, "turntableEnabled", loadedProject.lookDevViewSettings.turntableEnabled);
            loadedProject.lookDevViewSettings.turntableSpeed = static_cast<float>(JsonNumberOr(*view, "turntableSpeed", loadedProject.lookDevViewSettings.turntableSpeed));
        }

        if (const JsonValue* llm = FindMember(root, "llm"); llm && llm->type == JsonValue::Type::Object)
        {
            loadedLlm.modelPath = ResolveProjectPath(JsonStringOr(*llm, "modelPath"), projectDirectory);
            loadedLlm.contextTokens = static_cast<int>(JsonNumberOr(*llm, "contextTokens", loadedLlm.contextTokens));
            loadedLlm.maxTokens = static_cast<int>(JsonNumberOr(*llm, "maxTokens", loadedLlm.maxTokens));
            loadedLlm.gpuLayers = static_cast<int>(JsonNumberOr(*llm, "gpuLayers", loadedLlm.gpuLayers));
            loadedLlm.threads = static_cast<int>(JsonNumberOr(*llm, "threads", loadedLlm.threads));
            loadedLlm.temperature = static_cast<float>(JsonNumberOr(*llm, "temperature", loadedLlm.temperature));
            loadedLlm.topP = static_cast<float>(JsonNumberOr(*llm, "topP", loadedLlm.topP));
            loadedLlm.topK = static_cast<int>(JsonNumberOr(*llm, "topK", loadedLlm.topK));
        }

        if (const JsonValue* materials = FindMember(root, "materials"); materials && materials->type == JsonValue::Type::Array)
        {
            for (const JsonValue& materialValue : materials->array)
            {
                if (materialValue.type != JsonValue::Type::Object)
                {
                    continue;
                }
                rb::MaterialAssignment material;
                material.materialName = JsonStringOr(materialValue, "name", "Material");
                material.shaderSetName = "LookDev PBR";
                material.baseColorFactor = JsonFloat4Or(materialValue, "baseColorFactor", material.baseColorFactor);
                material.emissiveFactor = JsonFloat4Or(materialValue, "emissiveFactor", material.emissiveFactor);
                material.roughnessFactor = static_cast<float>(JsonNumberOr(materialValue, "roughnessFactor", material.roughnessFactor));
                material.metallicFactor = static_cast<float>(JsonNumberOr(materialValue, "metallicFactor", material.metallicFactor));
                material.normalStrength = static_cast<float>(JsonNumberOr(materialValue, "normalStrength", material.normalStrength));
                material.occlusionStrength = static_cast<float>(JsonNumberOr(materialValue, "occlusionStrength", material.occlusionStrength));
                material.alphaMode = AlphaModeFromString(JsonStringOr(materialValue, "alphaMode"), material.alphaMode);
                material.alphaCutoff = static_cast<float>(JsonNumberOr(materialValue, "alphaCutoff", material.alphaCutoff));
                material.packedOcclusionRoughnessMetallic = JsonBoolOr(materialValue, "packedOcclusionRoughnessMetallic", material.packedOcclusionRoughnessMetallic);
                material.flipNormalGreen = JsonBoolOr(materialValue, "flipNormalGreen", material.flipNormalGreen);
                if (const JsonValue* textures = FindMember(materialValue, "textures"); textures && textures->type == JsonValue::Type::Object)
                {
                    for (std::size_t slot = 0; slot < TextureSlotCount; ++slot)
                    {
                        if (const JsonValue* texture = FindMember(*textures, TextureSlotJsonNames[slot]); texture && texture->type == JsonValue::Type::Object)
                        {
                            material.textureOverrideEnabled[slot] = JsonBoolOr(*texture, "override", material.textureOverrideEnabled[slot]);
                            material.textureOverrides[slot] = ResolveProjectPath(JsonStringOr(*texture, "path"), projectDirectory).wstring();
                        }
                    }
                }
                loadedProject.materialAssignments.push_back(material);
            }
        }

        m_project = loadedProject;
        m_llmConfig = loadedLlm;
        if (!m_project.scenePath.empty() && PathExists(m_project.scenePath))
        {
            std::vector<rb::MaterialAssignment> savedMaterials = m_project.materialAssignments;
            const rb::ModelTransform savedModelTransform = m_project.modelTransform;
            LoadScenePath(m_project.scenePath);
            m_project.modelTransform = savedModelTransform;
            ApplyModelTransform();
            if (!savedMaterials.empty())
            {
                m_project.materialAssignments = std::move(savedMaterials);
                ApplyMaterialAssignments();
            }
        }
        else
        {
            UsePreviewScene();
            m_project.modelTransform = loadedProject.modelTransform;
            ApplyModelTransform();
            m_project.path = path.wstring();
        }

        std::string envDiagnostics;
        if (!m_project.lookDevEnvironment.environmentPath.empty() && PathExists(m_project.lookDevEnvironment.environmentPath))
        {
            m_backend.UpdateEnvironmentTexture(m_project.lookDevEnvironment.environmentPath, envDiagnostics);
        }
        else
        {
            m_project.lookDevEnvironment.environmentPath.clear();
            m_backend.UpdateEnvironmentTexture({}, envDiagnostics);
        }
        ApplyLookDevSettings();
        if (m_project.hasViewportCamera)
        {
            m_backend.SetCameraState(m_project.viewportCamera);
        }

        m_project.path = path.wstring();
        m_projectDirty = false;
        m_projectDiagnostics = "Project loaded.";
        return true;
    }
    catch (const std::exception& ex)
    {
        m_projectDiagnostics = "Project load failed: " + std::string(ex.what());
        return false;
    }
}

void ChatLookDevApp::LoadLocalModel()
{
    if (!PathExists(m_llmConfig.modelPath))
    {
        m_lastActionDiagnostics = "GGUF model file was not found.";
        return;
    }
    if (m_llm.LoadAsync(m_llmConfig))
    {
        m_chatMessages.push_back({ "system", "Loading local GGUF model." });
        m_scrollChatToBottom = true;
    }
}

void ChatLookDevApp::SendChatPrompt()
{
    const std::string prompt = TrimAscii(m_chatInput.data());
    if (prompt.empty())
    {
        return;
    }
    std::vector<LocalLlmMessage> messages = BuildLlmMessages(prompt);
    if (m_llm.Submit(messages))
    {
        m_chatMessages.push_back({ "user", prompt });
        std::fill(m_chatInput.begin(), m_chatInput.end(), '\0');
        m_scrollChatToBottom = true;
    }
}

std::vector<LocalLlmMessage> ChatLookDevApp::BuildLlmMessages(const std::string& userText) const
{
    std::vector<LocalLlmMessage> messages;
    messages.push_back({ "system", BuildSystemPrompt() });
    const std::size_t historyStart = m_chatMessages.size() > 8 ? m_chatMessages.size() - 8 : 0;
    for (std::size_t i = historyStart; i < m_chatMessages.size(); ++i)
    {
        if (m_chatMessages[i].role == "user" || m_chatMessages[i].role == "assistant")
        {
            messages.push_back({ m_chatMessages[i].role, m_chatMessages[i].text });
        }
    }
    messages.push_back({ "user", userText });
    return messages;
}

std::string ChatLookDevApp::BuildSystemPrompt() const
{
    std::ostringstream prompt;
    prompt << "You are a local LookDev assistant embedded in a Direct3D 12 PBR viewer.\n";
    prompt << "Reply in the user's language inside the JSON reply field.\n";
    prompt << "Return only strict JSON, no markdown, no prose outside JSON.\n";
    prompt << "Schema: {\"reply\":\"short user-facing reply\",\"actions\":[{\"method\":\"set_view_settings|set_environment_settings|set_sun_settings|set_material_preview|set_camera|set_model_transform\",\"params\":{}}]}.\n";
    prompt << "Allowed model transform params: translation float3 absolute, translationDelta float3 additive, rotationDegrees float3 absolute XYZ degrees, rotationDegreesDelta float3 additive XYZ degrees, rotationRadians float3 absolute, rotationRadiansDelta float3 additive, reset boolean.\n";
    prompt << "Allowed view params: exposure, gamma, toneMapper(None/Reinhard/ACES), displayMode(Beauty/Base Color/Normal/Roughness/Metallic/Ambient Occlusion/Emissive/Lighting Only), turntableEnabled, turntableSpeed.\n";
    prompt << "Allowed environment params: rotationYaw, intensity, backgroundMode(Sky Color/HDRI/Checker), skyTopColor, skyHorizonColor.\n";
    prompt << "Allowed sun params: sunDirection float3, sunColor float3, illuminanceLux.\n";
    prompt << "Allowed material params: materialName optional, baseColorFactor float4, emissiveFactor float4, roughnessFactor, metallicFactor, normalStrength, occlusionStrength, alphaMode(Opaque/Mask/Blend), alphaCutoff, packedOcclusionRoughnessMetallic, flipNormalGreen.\n";
    prompt << "Allowed camera params: target float3, yaw, pitch, distance.\n";
    prompt << "Do not request shader edits, MCP, automation, runtime compilation, DXR, path tracing, or external tools.\n";
    prompt << "Current state JSON: " << BuildControlStateJson();
    return prompt.str();
}

std::string ChatLookDevApp::BuildControlStateJson() const
{
    std::ostringstream json;
    const std::string selectedMaterial = m_project.materialAssignments.empty() ? "" : m_project.materialAssignments[std::min(m_selectedMaterial, m_project.materialAssignments.size() - 1)].materialName;
    const rb::ViewportCamera camera = m_backend.CameraState();
    json << "{";
    json << "\"scenePath\":\"" << EscapeJson(PathToUtf8(m_project.scenePath)) << "\",";
    json << "\"selectedMaterial\":\"" << EscapeJson(selectedMaterial) << "\",";
    json << "\"modelTransform\":{\"translation\":" << Float3Json(m_project.modelTransform.translation) << ",\"rotationDegrees\":" << Float3Json(m_project.modelTransform.rotationDegrees) << "},";
    json << "\"camera\":{\"target\":" << Float3Json(camera.target) << ",\"yaw\":" << camera.yaw << ",\"pitch\":" << camera.pitch << ",\"distance\":" << camera.distance << "},";
    json << "\"environment\":{\"hdriPath\":\"" << EscapeJson(PathToUtf8(m_project.lookDevEnvironment.environmentPath)) << "\",\"rotationYaw\":" << m_project.lookDevEnvironment.rotationYaw << ",\"intensity\":" << m_project.lookDevEnvironment.intensity << ",\"backgroundMode\":\"" << BackgroundModeName(m_project.lookDevEnvironment.backgroundMode) << "\",\"sunDirection\":" << Float3Json(m_project.lookDevEnvironment.sunDirection) << ",\"sunColor\":" << Float3Json(m_project.lookDevEnvironment.sunColor) << ",\"illuminanceLux\":" << m_project.lookDevEnvironment.sunIntensity << "},";
    json << "\"view\":{\"exposure\":" << m_project.lookDevViewSettings.exposure << ",\"gamma\":" << m_project.lookDevViewSettings.gamma << ",\"toneMapper\":\"" << ToneMapperName(m_project.lookDevViewSettings.toneMapper) << "\",\"displayMode\":\"" << DisplayModeName(m_project.lookDevViewSettings.displayMode) << "\"},";
    json << "\"materials\":[";
    for (std::size_t i = 0; i < m_project.materialAssignments.size(); ++i)
    {
        if (i > 0)
        {
            json << ",";
        }
        const rb::MaterialAssignment& material = m_project.materialAssignments[i];
        json << "{\"name\":\"" << EscapeJson(material.materialName) << "\",\"roughnessFactor\":" << material.roughnessFactor << ",\"metallicFactor\":" << material.metallicFactor << ",\"baseColorFactor\":" << Float4Json(material.baseColorFactor) << "}";
    }
    json << "]}";
    return json.str();
}

void ChatLookDevApp::DrainLlmEvents()
{
    for (const LocalLlmEvent& event : m_llm.DrainEvents())
    {
        if (event.kind == LocalLlmEvent::Kind::Response)
        {
            HandleLlmResponse(event.text);
        }
        else if (event.kind == LocalLlmEvent::Kind::Error)
        {
            m_chatMessages.push_back({ "system", event.text });
            m_scrollChatToBottom = true;
        }
        else
        {
            m_lastActionDiagnostics = event.text;
        }
    }
}

void ChatLookDevApp::HandleLlmResponse(const std::string& text)
{
    try
    {
        const std::string trimmed = TrimAscii(text);
        std::string jsonText;
        if (!ExtractFirstJsonObject(trimmed, jsonText))
        {
            throw std::runtime_error("AI response did not contain a JSON object.");
        }

        const bool recoveredFromWrappedOutput = jsonText.size() != trimmed.size();
        const JsonValue root = JsonParser(jsonText).Parse();
        if (root.type != JsonValue::Type::Object)
        {
            throw std::runtime_error("AI response root must be a JSON object.");
        }
        const JsonValue* replyValue = FindMember(root, "reply");
        if (!replyValue || replyValue->type != JsonValue::Type::String)
        {
            throw std::runtime_error("AI response must contain string field 'reply'.");
        }

        std::ostringstream actionDiagnostics;
        int appliedCount = 0;
        if (const JsonValue* actions = FindMember(root, "actions"))
        {
            if (actions->type != JsonValue::Type::Array)
            {
                throw std::runtime_error("'actions' must be an array.");
            }
            for (const JsonValue& action : actions->array)
            {
                if (action.type != JsonValue::Type::Object)
                {
                    actionDiagnostics << "Rejected non-object action. ";
                    continue;
                }
                const std::string method = JsonStringOr(action, "method");
                const JsonValue* params = FindMember(action, "params");
                JsonValue emptyParams;
                emptyParams.type = JsonValue::Type::Object;
                if (!params)
                {
                    params = &emptyParams;
                }
                if (params->type != JsonValue::Type::Object)
                {
                    actionDiagnostics << "Rejected action with non-object params. ";
                    continue;
                }
                std::string diagnostics;
                if (ApplyAiAction(method, *params, diagnostics))
                {
                    ++appliedCount;
                }
                else
                {
                    actionDiagnostics << diagnostics << " ";
                }
            }
        }

        m_chatMessages.push_back({ "assistant", replyValue->string });
        actionDiagnostics << "Applied actions: " << appliedCount << ".";
        if (recoveredFromWrappedOutput)
        {
            actionDiagnostics << " Ignored non-JSON text around the response.";
        }
        m_lastActionDiagnostics = actionDiagnostics.str();
        m_scrollChatToBottom = true;
    }
    catch (const std::exception& ex)
    {
        m_chatMessages.push_back({ "system", "Rejected invalid AI JSON: " + std::string(ex.what()) });
        m_lastActionDiagnostics = "Rejected invalid AI JSON.";
        m_scrollChatToBottom = true;
    }
}

bool ChatLookDevApp::ApplyAiAction(const std::string& method, const JsonValue& params, std::string& diagnostics)
{
    if (method == "set_model_transform")
    {
        rb::ModelTransform transform = m_project.modelTransform;
        if (JsonBoolOr(params, "reset", false))
        {
            transform = {};
        }

        if (!ReadOptionalFloat3(params, "translation", -1000000.0f, 1000000.0f, transform.translation, diagnostics)) return false;
        std::array<float, 3> translationDelta = { 0.0f, 0.0f, 0.0f };
        if (!ReadOptionalFloat3(params, "translationDelta", -1000000.0f, 1000000.0f, translationDelta, diagnostics)) return false;
        for (std::size_t i = 0; i < 3; ++i)
        {
            transform.translation[i] = std::clamp(transform.translation[i] + translationDelta[i], -1000000.0f, 1000000.0f);
        }

        if (!ReadOptionalFloat3(params, "rotationDegrees", -36000.0f, 36000.0f, transform.rotationDegrees, diagnostics)) return false;
        std::array<float, 3> rotationDegreesDelta = { 0.0f, 0.0f, 0.0f };
        if (!ReadOptionalFloat3(params, "rotationDegreesDelta", -36000.0f, 36000.0f, rotationDegreesDelta, diagnostics)) return false;
        for (std::size_t i = 0; i < 3; ++i)
        {
            transform.rotationDegrees[i] = std::clamp(transform.rotationDegrees[i] + rotationDegreesDelta[i], -36000.0f, 36000.0f);
        }

        if (const JsonValue* rotationRadians = FindMember(params, "rotationRadians"))
        {
            if (rotationRadians->type != JsonValue::Type::Array || rotationRadians->array.size() != 3)
            {
                diagnostics = "rotationRadians must be a float3 array.";
                return false;
            }
            std::array<float, 3> radians = { 0.0f, 0.0f, 0.0f };
            if (!ReadOptionalFloat3(params, "rotationRadians", -628.31854f, 628.31854f, radians, diagnostics)) return false;
            transform.rotationDegrees =
            {
                radians[0] * 57.2957795f,
                radians[1] * 57.2957795f,
                radians[2] * 57.2957795f,
            };
        }

        if (const JsonValue* rotationRadiansDelta = FindMember(params, "rotationRadiansDelta"))
        {
            if (rotationRadiansDelta->type != JsonValue::Type::Array || rotationRadiansDelta->array.size() != 3)
            {
                diagnostics = "rotationRadiansDelta must be a float3 array.";
                return false;
            }
            std::array<float, 3> radiansDelta = { 0.0f, 0.0f, 0.0f };
            if (!ReadOptionalFloat3(params, "rotationRadiansDelta", -628.31854f, 628.31854f, radiansDelta, diagnostics)) return false;
            for (std::size_t i = 0; i < 3; ++i)
            {
                transform.rotationDegrees[i] = std::clamp(transform.rotationDegrees[i] + radiansDelta[i] * 57.2957795f, -36000.0f, 36000.0f);
            }
        }

        m_project.modelTransform = transform;
        ApplyModelTransform();
        MarkProjectDirty();
        return true;
    }

    if (method == "set_view_settings")
    {
        rb::LookDevViewSettings view = m_project.lookDevViewSettings;
        if (!ReadOptionalNumber(params, "exposure", -16.0f, 16.0f, view.exposure, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "gamma", 0.1f, 5.0f, view.gamma, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "turntableSpeed", -10.0f, 10.0f, view.turntableSpeed, diagnostics)) return false;
        if (!ReadOptionalBool(params, "turntableEnabled", view.turntableEnabled, diagnostics)) return false;
        if (const JsonValue* toneMapper = FindMember(params, "toneMapper"))
        {
            if (toneMapper->type != JsonValue::Type::String)
            {
                diagnostics = "toneMapper must be a string.";
                return false;
            }
            view.toneMapper = ToneMapperFromString(toneMapper->string, view.toneMapper);
        }
        if (const JsonValue* displayMode = FindMember(params, "displayMode"))
        {
            if (displayMode->type != JsonValue::Type::String)
            {
                diagnostics = "displayMode must be a string.";
                return false;
            }
            view.displayMode = DisplayModeFromString(displayMode->string, view.displayMode);
        }
        m_project.lookDevViewSettings = view;
        ApplyLookDevSettings();
        MarkProjectDirty();
        return true;
    }

    if (method == "set_environment_settings")
    {
        rb::LookDevEnvironment environment = m_project.lookDevEnvironment;
        std::array<float, 4> skyTop = m_project.skyTopColor;
        std::array<float, 4> skyHorizon = m_project.skyHorizonColor;
        if (!ReadOptionalNumber(params, "rotationYaw", -6.2831855f, 6.2831855f, environment.rotationYaw, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "intensity", 0.0f, 32.0f, environment.intensity, diagnostics)) return false;
        if (!ReadOptionalFloat4(params, "skyTopColor", 0.0f, 16.0f, skyTop, diagnostics)) return false;
        if (!ReadOptionalFloat4(params, "skyHorizonColor", 0.0f, 16.0f, skyHorizon, diagnostics)) return false;
        if (const JsonValue* backgroundMode = FindMember(params, "backgroundMode"))
        {
            if (backgroundMode->type != JsonValue::Type::String)
            {
                diagnostics = "backgroundMode must be a string.";
                return false;
            }
            environment.backgroundMode = BackgroundModeFromString(backgroundMode->string, environment.backgroundMode);
        }
        m_project.lookDevEnvironment = environment;
        m_project.skyTopColor = skyTop;
        m_project.skyHorizonColor = skyHorizon;
        ApplyLookDevSettings();
        MarkProjectDirty();
        return true;
    }

    if (method == "set_sun_settings")
    {
        rb::LookDevEnvironment environment = m_project.lookDevEnvironment;
        if (!ReadOptionalFloat3(params, "sunDirection", -1.0f, 1.0f, environment.sunDirection, diagnostics)) return false;
        if (!ReadOptionalFloat3(params, "sunColor", 0.0f, 10.0f, environment.sunColor, diagnostics)) return false;
        float illuminanceLux = environment.sunIntensity;
        if (!ReadOptionalNumber(params, "illuminanceLux", 0.0f, 200000.0f, illuminanceLux, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "sunIntensity", 0.0f, 200000.0f, illuminanceLux, diagnostics)) return false;
        environment.sunIntensity = illuminanceLux;
        m_project.lookDevEnvironment = environment;
        ApplyLookDevSettings();
        MarkProjectDirty();
        return true;
    }

    if (method == "set_camera")
    {
        rb::ViewportCamera camera = m_backend.CameraState();
        if (!ReadOptionalFloat3(params, "target", -1000000.0f, 1000000.0f, camera.target, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "yaw", -1000.0f, 1000.0f, camera.yaw, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "pitch", -1.55f, 1.55f, camera.pitch, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "distance", 0.001f, 10000000.0f, camera.distance, diagnostics)) return false;
        m_backend.SetCameraState(camera);
        m_project.viewportCamera = camera;
        m_project.hasViewportCamera = true;
        MarkProjectDirty();
        return true;
    }

    if (method == "set_material_preview")
    {
        EnsureMaterialSelection();
        std::string materialName = JsonStringOr(params, "materialName");
        if (materialName.empty() && !m_project.materialAssignments.empty())
        {
            materialName = m_project.materialAssignments[m_selectedMaterial].materialName;
        }
        auto materialIt = std::find_if(m_project.materialAssignments.begin(), m_project.materialAssignments.end(), [&](const rb::MaterialAssignment& material) {
            return material.materialName == materialName;
        });
        if (materialIt == m_project.materialAssignments.end())
        {
            diagnostics = "materialName was not found.";
            return false;
        }
        rb::MaterialAssignment material = *materialIt;
        if (!ReadOptionalFloat4(params, "baseColorFactor", 0.0f, 16.0f, material.baseColorFactor, diagnostics)) return false;
        if (!ReadOptionalFloat4(params, "emissiveFactor", 0.0f, 1000.0f, material.emissiveFactor, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "roughnessFactor", 0.0f, 1.0f, material.roughnessFactor, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "metallicFactor", 0.0f, 1.0f, material.metallicFactor, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "normalStrength", 0.0f, 2.0f, material.normalStrength, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "occlusionStrength", 0.0f, 1.0f, material.occlusionStrength, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "alphaCutoff", 0.0f, 1.0f, material.alphaCutoff, diagnostics)) return false;
        if (!ReadOptionalBool(params, "packedOcclusionRoughnessMetallic", material.packedOcclusionRoughnessMetallic, diagnostics)) return false;
        if (!ReadOptionalBool(params, "flipNormalGreen", material.flipNormalGreen, diagnostics)) return false;
        if (const JsonValue* alphaMode = FindMember(params, "alphaMode"))
        {
            if (alphaMode->type != JsonValue::Type::String)
            {
                diagnostics = "alphaMode must be a string.";
                return false;
            }
            material.alphaMode = AlphaModeFromString(alphaMode->string, material.alphaMode);
        }
        *materialIt = material;
        m_selectedMaterial = static_cast<std::size_t>(std::distance(m_project.materialAssignments.begin(), materialIt));
        ApplyMaterialAssignments();
        MarkProjectDirty();
        return true;
    }

    diagnostics = "Unsupported action method: " + method + ".";
    return false;
}

LRESULT CALLBACK ChatLookDevApp::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
    {
        return true;
    }

    ChatLookDevApp* app = nullptr;
    if (message == WM_NCCREATE)
    {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lparam);
        app = static_cast<ChatLookDevApp*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<ChatLookDevApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app)
    {
        return app->HandleMessage(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ChatLookDevApp::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_SIZE:
        if (m_backend.Device() && wparam != SIZE_MINIMIZED)
        {
            const UINT width = LOWORD(lparam);
            const UINT height = HIWORD(lparam);
            m_backend.Resize(width, height);
        }
        return 0;
    case WM_CLOSE:
        m_running = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (m_hwnd == hwnd)
        {
            m_hwnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
}
