#include "ChatLookDevApp.h"

#include "SimpleJson.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cctype>
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
constexpr std::size_t MaxPromptMaterialPreviewCount = 16;
constexpr float RadToDeg = 57.2957795f;
constexpr float DegToRad = 0.0174532925f;

struct SunAngles
{
    float azimuthDegrees = 0.0f;
    float elevationDegrees = 45.0f;
};

struct CameraPreset
{
    const char* name = "";
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
};

constexpr CameraPreset CameraPresets[] =
{
    { "Front", 0.0f, 0.0f },
    { "Back", 180.0f, 0.0f },
    { "Left", -90.0f, 0.0f },
    { "Right", 90.0f, 0.0f },
    { "Top", 0.0f, 88.0f },
    { "Bottom", 0.0f, -88.0f },
    { "Iso", 45.0f, 35.0f },
};

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

std::string LowerAscii(std::string text)
{
    for (char& ch : text)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

const CameraPreset* FindCameraPreset(const std::string& name)
{
    const std::string normalized = LowerAscii(cld::TrimAscii(name));
    for (const CameraPreset& preset : CameraPresets)
    {
        const std::string presetName = LowerAscii(preset.name);
        if (normalized == presetName)
        {
            return &preset;
        }
    }
    if (normalized == "isometric")
    {
        return &CameraPresets[6];
    }
    return nullptr;
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

bool ReadOptionalBookmarkSlot(const cld::JsonValue& value, const char* name, int& target, std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(value, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Number)
    {
        diagnostics = std::string(name) + " must be a number in range 1..3.";
        return false;
    }
    const int slot = static_cast<int>(std::lround(member->number));
    if (std::abs(member->number - static_cast<double>(slot)) > 0.001 || slot < 1 || slot > static_cast<int>(rb::CameraBookmarkCount))
    {
        diagnostics = std::string(name) + " must be a number in range 1..3.";
        return false;
    }
    target = slot;
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

std::string CameraJson(const rb::ViewportCamera& camera)
{
    std::ostringstream json;
    json << "{ "
         << "\"target\": " << Float3Json(camera.target) << ", "
         << "\"yaw\": " << camera.yaw << ", "
         << "\"pitch\": " << camera.pitch << ", "
         << "\"distance\": " << camera.distance << ", "
         << "\"fovDegrees\": " << camera.fovDegrees << " }";
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

float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> Cross3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return
    {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };
}

std::array<float, 3> Normalized3(std::array<float, 3> value)
{
    NormalizeDirection(value);
    return value;
}

SunAngles SunAnglesFromDirection(std::array<float, 3> rayDirection)
{
    NormalizeDirection(rayDirection);
    const std::array<float, 3> source =
    {
        -rayDirection[0],
        -rayDirection[1],
        -rayDirection[2],
    };

    SunAngles angles;
    angles.azimuthDegrees = std::atan2(source[0], source[2]) * RadToDeg;
    angles.elevationDegrees = std::asin(std::clamp(source[1], -1.0f, 1.0f)) * RadToDeg;
    return angles;
}

std::array<float, 3> SunDirectionFromAngles(float azimuthDegrees, float elevationDegrees)
{
    const float azimuth = azimuthDegrees * DegToRad;
    const float elevation = std::clamp(elevationDegrees, -5.0f, 89.0f) * DegToRad;
    const float cosElevation = std::cos(elevation);
    const std::array<float, 3> source =
    {
        std::sin(azimuth) * cosElevation,
        std::sin(elevation),
        std::cos(azimuth) * cosElevation,
    };
    return { -source[0], -source[1], -source[2] };
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

std::uint32_t NormalizeShadowResolution(std::uint32_t resolution)
{
    // The renderer only allocates fixed-size shadow maps; clamp chat/project/UI
    // input to the same buckets before any state is committed.
    if (resolution <= 1024)
    {
        return 1024;
    }
    if (resolution <= 2048)
    {
        return 2048;
    }
    return 4096;
}

float UiSettingNumberOrDefault(const cld::JsonValue& value, const char* name, float defaultValue, float minValue, float maxValue, std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(value, name);
    if (!member)
    {
        return defaultValue;
    }
    if (member->type != cld::JsonValue::Type::Number || member->number < minValue || member->number > maxValue)
    {
        if (!diagnostics.empty())
        {
            diagnostics += " ";
        }
        diagnostics += std::string(name) + " was outside the allowed UI settings range.";
        return defaultValue;
    }
    return static_cast<float>(member->number);
}

rb::UiSettings ClampUiSettings(rb::UiSettings settings)
{
    settings.uiFontSize = std::clamp(settings.uiFontSize, 14.0f, 28.0f);
    settings.chatFontSize = std::clamp(settings.chatFontSize, 16.0f, 32.0f);
    settings.uiScale = std::clamp(settings.uiScale, 0.85f, 1.5f);
    settings.chatTranscriptHeightRatio = std::clamp(settings.chatTranscriptHeightRatio, 0.35f, 0.70f);
    return settings;
}

bool IsSupportedAiAction(const std::string& method)
{
    return method == "set_view_settings"
        || method == "set_environment_settings"
        || method == "set_sun_settings"
        || method == "set_shadow_settings"
        || method == "set_material_preview"
        || method == "set_camera"
        || method == "set_model_transform";
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
    LoadUiSettings();

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
    m_backend.Initialize(m_hwnd, 1600, 980, m_rootDirectory, m_uiSettings);
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
    m_project.lookDevShadowSettings = {};
    m_project.lookDevShadowSettings.resolution = NormalizeShadowResolution(m_project.lookDevShadowSettings.resolution);
    m_project.materialAssignments.clear();
    m_project.materialAssignments.push_back(rb::MaterialAssignment{ "Default Material", "LookDev PBR" });

    m_llmConfig.modelPath = m_rootDirectory / "Assets" / "Models" / "gemma-4-E4B-it" / "gemma-4-E4B-it-Q4_K_M.gguf";
    m_llmConfig.contextTokens = 4096;
    m_llmConfig.maxTokens = 512;
    m_llmConfig.gpuLayers = 1;
    m_llmConfig.threads = 1;
    m_llmConfig.temperature = 0.2f;
    m_llmConfig.topP = 0.9f;
    m_llmConfig.topK = 40;
    m_llmConfig.structuredJson = true;
    m_llmConfig.mtpEnabled = false;
    m_llmConfig.mtpDraftTokens = 4;
    m_llmConfig.mtpMinP = 0.0f;
    m_llmConfig.mtpBackendSampling = true;
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
    DrawUiSettingsPanel();
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
        ImGui::Separator();
        if (ImGui::MenuItem("UI Settings"))
        {
            m_showUiSettings = true;
        }
        if (ImGui::MenuItem("Reset UI Layout"))
        {
            ResetUiLayout();
        }
        if (ImGui::MenuItem("Reset UI Settings"))
        {
            ResetUiSettings();
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
            FrameSceneCamera();
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
    DrawCameraOverlay(imagePos, ImVec2(imagePos.x + width, imagePos.y + height));
    DrawSunDirectionOverlay(imagePos, ImVec2(imagePos.x + width, imagePos.y + height));
    HandleViewportInput(imagePos, ImVec2(imagePos.x + width, imagePos.y + height));
    ImGui::End();
}

void ChatLookDevApp::DrawCameraOverlay(const ImVec2& imageMin, const ImVec2& imageMax)
{
    if (!m_showCameraOverlay)
    {
        return;
    }

    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width < 280.0f || height < 140.0f)
    {
        return;
    }

    const rb::ViewportCamera camera = m_backend.CameraState();
    const float yawDegrees = camera.yaw * RadToDeg;
    const float pitchDegrees = camera.pitch * RadToDeg;
    std::array<std::string, 4> lines;
    {
        std::ostringstream text;
        text << "FOV " << std::fixed << std::setprecision(1) << camera.fovDegrees
             << "  Dist " << std::setprecision(2) << camera.distance;
        lines[0] = text.str();
    }
    {
        std::ostringstream text;
        text << "Yaw " << std::fixed << std::setprecision(1) << yawDegrees
             << "  Pitch " << pitchDegrees;
        lines[1] = text.str();
    }
    {
        std::ostringstream text;
        text << "Target " << std::fixed << std::setprecision(2)
             << camera.target[0] << ", " << camera.target[1] << ", " << camera.target[2];
        lines[2] = text.str();
    }
    lines[3] = "Camera";

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin(imageMin.x + 16.0f, imageMin.y + 16.0f);
    const ImVec2 padding(10.0f, 8.0f);
    float boxWidth = 0.0f;
    const float lineHeight = ImGui::GetTextLineHeight();
    for (const std::string& line : lines)
    {
        boxWidth = std::max(boxWidth, ImGui::CalcTextSize(line.c_str()).x);
    }
    const ImVec2 boxMin = origin;
    const ImVec2 boxMax(origin.x + boxWidth + padding.x * 2.0f, origin.y + lineHeight * 4.0f + padding.y * 2.0f + 6.0f);
    drawList->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImVec4(0.02f, 0.025f, 0.03f, 0.68f)), 6.0f);
    drawList->AddRect(boxMin, boxMax, ImGui::GetColorU32(ImVec4(0.65f, 0.70f, 0.78f, 0.34f)), 6.0f);

    const ImU32 labelColor = ImGui::GetColorU32(ImVec4(0.55f, 0.78f, 1.0f, 0.96f));
    const ImU32 textColor = ImGui::GetColorU32(ImVec4(0.92f, 0.95f, 1.0f, 0.94f));
    drawList->AddText(ImVec2(origin.x + padding.x, origin.y + padding.y), labelColor, lines[3].c_str());
    for (std::size_t i = 0; i < 3; ++i)
    {
        drawList->AddText(ImVec2(origin.x + padding.x, origin.y + padding.y + lineHeight * static_cast<float>(i + 1) + 2.0f), textColor, lines[i].c_str());
    }
}

void ChatLookDevApp::DrawSunDirectionOverlay(const ImVec2& imageMin, const ImVec2& imageMax)
{
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width < 180.0f || height < 140.0f)
    {
        return;
    }

    const rb::ViewportCamera camera = m_backend.CameraState();
    const float radius = 34.0f;
    const ImVec2 center(imageMax.x - radius - 18.0f, imageMin.y + radius + 18.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const ImU32 background = ImGui::GetColorU32(ImVec4(0.02f, 0.025f, 0.03f, 0.68f));
    const ImU32 ring = ImGui::GetColorU32(ImVec4(0.85f, 0.88f, 0.92f, 0.55f));
    const ImU32 axis = ImGui::GetColorU32(ImVec4(0.65f, 0.70f, 0.78f, 0.36f));
    const ImU32 sun = ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.30f, 1.0f));
    const ImU32 text = ImGui::GetColorU32(ImVec4(1.0f, 0.92f, 0.70f, 0.95f));

    drawList->AddCircleFilled(center, radius + 9.0f, background, 40);
    drawList->AddCircle(center, radius, ring, 40, 1.25f);
    drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), axis, 1.0f);
    drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), axis, 1.0f);

    std::array<float, 3> source =
    {
        -m_project.lookDevEnvironment.sunDirection[0],
        -m_project.lookDevEnvironment.sunDirection[1],
        -m_project.lookDevEnvironment.sunDirection[2],
    };
    source = Normalized3(source);

    const float cp = std::cos(camera.pitch);
    const std::array<float, 3> forward = Normalized3({ -std::sin(camera.yaw) * cp, -std::sin(camera.pitch), -std::cos(camera.yaw) * cp });
    const std::array<float, 3> right = Normalized3({ std::cos(camera.yaw), 0.0f, -std::sin(camera.yaw) });
    const std::array<float, 3> up = Normalized3(Cross3(forward, right));
    const float sx = Dot3(source, right);
    const float sy = Dot3(source, up);
    const float facing = Dot3(source, forward);

    const ImVec2 end(center.x + sx * radius * 0.78f, center.y - sy * radius * 0.78f);
    const ImVec2 vector(end.x - center.x, end.y - center.y);
    const float vectorLength = std::max(std::sqrt(vector.x * vector.x + vector.y * vector.y), 0.001f);
    const ImVec2 unit(vector.x / vectorLength, vector.y / vectorLength);
    const ImVec2 perp(-unit.y, unit.x);
    const float headLength = 9.0f;
    const float headWidth = 5.0f;
    const ImVec2 base(end.x - unit.x * headLength, end.y - unit.y * headLength);

    drawList->AddLine(center, end, sun, 2.4f);
    drawList->AddTriangleFilled(
        end,
        ImVec2(base.x + perp.x * headWidth, base.y + perp.y * headWidth),
        ImVec2(base.x - perp.x * headWidth, base.y - perp.y * headWidth),
        sun);
    drawList->AddCircleFilled(center, 2.6f, ring, 12);
    drawList->AddCircleFilled(end, facing >= 0.0f ? 4.0f : 2.6f, sun, 14);
    drawList->AddText(ImVec2(center.x - radius + 2.0f, center.y + radius + 4.0f), text, "Sun");
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
        AddScaled(m_project.modelTransform.translation, cameraForward, -moveScale);
        modelChanged = true;
    }
    if (ImGui::IsKeyDown(ImGuiKey_S))
    {
        AddScaled(m_project.modelTransform.translation, cameraForward, moveScale);
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
    const auto commitCameraEdit = [&]() {
        m_project.viewportCamera = m_backend.CameraState();
        m_project.hasViewportCamera = true;
        MarkProjectDirty();
    };

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        // Blender-style viewport navigation: MMB orbit, Shift+MMB pan,
        // Ctrl+MMB dolly. Left/right drag bindings remain as convenient aliases.
        if (io.KeyCtrl)
        {
            m_backend.DollyCamera(-delta.y * 0.05f);
        }
        else if (io.KeyShift)
        {
            m_backend.PanCamera(delta.x * 0.002f, delta.y * 0.002f);
        }
        else
        {
            m_backend.OrbitCamera(delta.x * 0.006f, delta.y * 0.006f);
        }
        commitCameraEdit();
    }
    else if (modelMouseMode && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        m_project.modelTransform.rotationDegrees[1] += delta.x * 0.35f;
        m_project.modelTransform.rotationDegrees[0] += delta.y * 0.35f;
        modelChanged = true;
    }
    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        m_backend.OrbitCamera(delta.x * 0.006f, delta.y * 0.006f);
        commitCameraEdit();
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
        commitCameraEdit();
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
            commitCameraEdit();
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

    ImGui::Separator();
    ImGui::TextUnformatted("Camera");
    rb::ViewportCamera camera = m_backend.CameraState();
    float yawDegrees = camera.yaw * RadToDeg;
    float pitchDegrees = camera.pitch * RadToDeg;
    bool cameraChanged = false;
    ImGui::Checkbox("Show Camera Overlay", &m_showCameraOverlay);
    cameraChanged |= ImGui::DragFloat3("Target", camera.target.data(), 0.01f, -1000000.0f, 1000000.0f, "%.3f");
    cameraChanged |= ImGui::DragFloat("Yaw deg", &yawDegrees, 0.1f, -36000.0f, 36000.0f, "%.2f");
    cameraChanged |= ImGui::DragFloat("Pitch deg", &pitchDegrees, 0.1f, -88.81f, 88.81f, "%.2f");
    cameraChanged |= ImGui::DragFloat("Distance", &camera.distance, 0.05f, 0.001f, 10000000.0f, "%.3f");
    cameraChanged |= ImGui::SliderFloat("FOV deg", &camera.fovDegrees, 5.0f, 120.0f, "%.1f");
    if (cameraChanged)
    {
        camera.yaw = yawDegrees * DegToRad;
        camera.pitch = std::clamp(pitchDegrees * DegToRad, -1.55f, 1.55f);
        camera.distance = std::max(camera.distance, 0.001f);
        camera.fovDegrees = std::clamp(camera.fovDegrees, 5.0f, 120.0f);
        CommitCameraState(camera);
    }
    if (ImGui::Button("Frame Scene"))
    {
        FrameSceneCamera();
    }
    ImGui::SameLine();
    if (ImGui::Button("Front"))
    {
        ApplyCameraPreset("Front", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Back"))
    {
        ApplyCameraPreset("Back", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Iso"))
    {
        ApplyCameraPreset("Iso", true);
    }
    if (ImGui::Button("Left"))
    {
        ApplyCameraPreset("Left", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Right"))
    {
        ApplyCameraPreset("Right", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Top"))
    {
        ApplyCameraPreset("Top", true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Bottom"))
    {
        ApplyCameraPreset("Bottom", true);
    }

    ImGui::SeparatorText("Camera Bookmarks");
    for (std::size_t slot = 0; slot < rb::CameraBookmarkCount; ++slot)
    {
        ImGui::PushID(static_cast<int>(slot));
        ImGui::Text("%zu", slot + 1);
        ImGui::SameLine();
        if (ImGui::Button("Store"))
        {
            StoreCameraBookmark(slot);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_project.cameraBookmarks[slot].valid);
        if (ImGui::Button("Recall"))
        {
            RecallCameraBookmark(slot);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            m_project.cameraBookmarks[slot] = {};
            MarkProjectDirty();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
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
    rb::LookDevShadowSettings& shadow = m_project.lookDevShadowSettings;
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
    SunAngles sunAngles = SunAnglesFromDirection(environment.sunDirection);
    bool sunAngleChanged = false;
    sunAngleChanged |= ImGui::SliderFloat("Azimuth deg", &sunAngles.azimuthDegrees, -180.0f, 180.0f, "%.1f");
    sunAngleChanged |= ImGui::SliderFloat("Elevation deg", &sunAngles.elevationDegrees, -5.0f, 89.0f, "%.1f");
    if (sunAngleChanged)
    {
        environment.sunDirection = SunDirectionFromAngles(sunAngles.azimuthDegrees, sunAngles.elevationDegrees);
        changed = true;
    }
    if (ImGui::Button("Noon"))
    {
        environment.sunDirection = SunDirectionFromAngles(0.0f, 80.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Side"))
    {
        environment.sunDirection = SunDirectionFromAngles(90.0f, 35.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rim"))
    {
        environment.sunDirection = SunDirectionFromAngles(180.0f, 25.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Low"))
    {
        environment.sunDirection = SunDirectionFromAngles(-45.0f, 10.0f);
        changed = true;
    }
    changed |= ImGui::ColorEdit3("Color", environment.sunColor.data());
    changed |= ImGui::DragFloat("Illuminance (lux)", &environment.sunIntensity, 100.0f, 0.0f, 200000.0f, "%.0f");

    ImGui::SeparatorText("Shadows");
    changed |= ImGui::Checkbox("Enable Shadows", &shadow.enabled);
    const std::uint32_t resolutions[] = { 1024, 2048, 4096 };
    int resolutionIndex = shadow.resolution <= 1024 ? 0 : (shadow.resolution <= 2048 ? 1 : 2);
    const char* resolutionLabels[] = { "1024", "2048", "4096" };
    if (ImGui::Combo("Shadow Resolution", &resolutionIndex, resolutionLabels, IM_ARRAYSIZE(resolutionLabels)))
    {
        shadow.resolution = resolutions[std::clamp(resolutionIndex, 0, 2)];
        changed = true;
    }
    changed |= ImGui::SliderFloat("Shadow Strength", &shadow.strength, 0.0f, 1.0f);
    changed |= ImGui::DragFloat("Shadow Bias", &shadow.bias, 0.0001f, 0.0f, 0.05f, "%.5f");
    changed |= ImGui::SliderFloat("Shadow Softness", &shadow.softness, 0.0f, 8.0f);
    changed |= ImGui::SliderFloat("Shadow Fit Scale", &shadow.fitScale, 1.0f, 4.0f);
    const bool showingShadowMask = view.displayMode == rb::LookDevDisplayMode::ShadowMask;
    if (ImGui::Button(showingShadowMask ? "Beauty View" : "Shadow Mask View"))
    {
        view.displayMode = showingShadowMask ? rb::LookDevDisplayMode::Beauty : rb::LookDevDisplayMode::ShadowMask;
        changed = true;
    }

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
    ImGui::TextWrapped("Inference: %s", status.inferenceMode.c_str());
    if (status.mtpRequested || status.mtpAvailable)
    {
        ImGui::TextWrapped("%s", status.mtpMode.c_str());
    }
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
        m_llm.CancelGeneration();
    }
    ImGui::SameLine();
    if (ImGui::Button("Unload Model"))
    {
        m_llm.Stop();
    }
    if (!PathExists(m_llmConfig.modelPath))
    {
        ImGui::TextWrapped("Default GGUF is not present. Place gemma-4-E4B-it-Q4_K_M.gguf under Assets/Models/gemma-4-E4B-it or browse to another GGUF.");
    }

    ImGui::SliderInt("Context Tokens", &m_llmConfig.contextTokens, 1024, 32768);
    ImGui::SliderInt("Max Reply Tokens", &m_llmConfig.maxTokens, 64, 2048);
    ImGui::SliderInt("CPU Threads", &m_llmConfig.threads, 1, 64);
    ImGui::SliderInt("GPU Layers", &m_llmConfig.gpuLayers, 0, 128);
    ImGui::Checkbox("Structured JSON", &m_llmConfig.structuredJson);
    ImGui::SliderFloat("Temperature", &m_llmConfig.temperature, 0.0f, 1.5f);
    ImGui::SliderFloat("Top P", &m_llmConfig.topP, 0.05f, 1.0f);
    bool mtpChanged = false;
    mtpChanged |= ImGui::Checkbox("MTP Draft", &m_llmConfig.mtpEnabled);
    if (m_llmConfig.mtpEnabled)
    {
        const std::string draftPath = PathToUtf8(m_llmConfig.mtpDraftModelPath);
        ImGui::BeginDisabled();
        ImGui::InputText("MTP Draft Model", const_cast<char*>(draftPath.c_str()), draftPath.size() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        if (ImGui::Button("Browse MTP Draft"))
        {
            const auto path = OpenFileDialog(L"GGUF Model\0*.gguf\0All Files\0*.*\0");
            if (!path.empty())
            {
                m_llmConfig.mtpDraftModelPath = path;
                mtpChanged = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear MTP Draft"))
        {
            m_llmConfig.mtpDraftModelPath.clear();
            mtpChanged = true;
        }
        mtpChanged |= ImGui::SliderInt("MTP Draft Tokens", &m_llmConfig.mtpDraftTokens, 1, 16);
        mtpChanged |= ImGui::SliderFloat("MTP Min P", &m_llmConfig.mtpMinP, 0.0f, 1.0f);
        mtpChanged |= ImGui::Checkbox("MTP Backend Sampling", &m_llmConfig.mtpBackendSampling);
    }
    if (mtpChanged)
    {
        MarkProjectDirty();
    }
    if (!m_lastLlmStats.empty())
    {
        ImGui::TextWrapped("%s", m_lastLlmStats.c_str());
    }

    ImGui::Separator();
    const float lowerPanelHeight = ImGui::GetContentRegionAvail().y;
    const float transcriptRatio = std::clamp(m_uiSettings.chatTranscriptHeightRatio, 0.35f, 0.70f);
    const float transcriptHeight = std::clamp(lowerPanelHeight * transcriptRatio, 160.0f, 420.0f);
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
    const float inputHeight = std::max(72.0f, ImGui::GetTextLineHeightWithSpacing() * 3.2f + ImGui::GetStyle().FramePadding.y * 2.0f);
    ImGui::InputTextMultiline("##ChatInput", m_chatInput.data(), m_chatInput.size(), ImVec2(-1.0f, inputHeight));
    const bool chatInputActive = ImGui::IsItemActive();
    ImGui::PopFont();
    const bool canSend = status.state == LocalLlmState::Ready && TrimAscii(m_chatInput.data()).size() > 0;
    const bool ctrlEnterSend = chatInputActive
        && ImGui::GetIO().KeyCtrl
        && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    ImGui::BeginDisabled(!canSend);
    if (ImGui::Button("Send") || (canSend && ctrlEnterSend))
    {
        SendChatPrompt();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        m_chatMessages.clear();
        m_aiHistory.clear();
        m_pendingUserPrompt.clear();
        m_lastActionDiagnostics.clear();
        m_lastLlmStats.clear();
        std::fill(m_chatInput.begin(), m_chatInput.end(), '\0');
        m_scrollChatToBottom = false;
    }
    if (!m_lastActionDiagnostics.empty())
    {
        ImGui::TextWrapped("%s", m_lastActionDiagnostics.c_str());
    }
    DrawActionHistoryPanel();
    ImGui::End();
}

void ChatLookDevApp::DrawActionHistoryPanel()
{
    if (!ImGui::CollapsingHeader("Action History", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    if (m_aiHistory.empty())
    {
        ImGui::TextUnformatted("No AI exchanges yet.");
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 3.0f));
    for (std::size_t offset = 0; offset < m_aiHistory.size(); ++offset)
    {
        const std::size_t index = m_aiHistory.size() - 1 - offset;
        const AiExchange& exchange = m_aiHistory[index];
        ImGui::PushID(static_cast<int>(index));
        std::ostringstream label;
        label << "#" << (index + 1) << " "
              << (exchange.rejectedReason.empty() ? "Applied" : "Rejected")
              << " (" << exchange.appliedCount << " action";
        if (exchange.appliedCount != 1)
        {
            label << "s";
        }
        label << ")";
        if (ImGui::TreeNode(label.str().c_str()))
        {
            ImGui::TextWrapped("User: %s", exchange.userPrompt.c_str());
            if (!exchange.assistantReply.empty())
            {
                ImGui::TextWrapped("Reply: %s", exchange.assistantReply.c_str());
            }
            if (!exchange.rejectedReason.empty())
            {
                ImGui::TextWrapped("Rejected: %s", exchange.rejectedReason.c_str());
            }
            if (!exchange.diagnostics.empty())
            {
                ImGui::TextWrapped("Diagnostics: %s", exchange.diagnostics.c_str());
            }
            ImGui::Text("Prompt Tokens: %d  Output Tokens: %d  Finish: %s  Grammar: %s  MTP: %s",
                exchange.promptTokens,
                exchange.outputTokens,
                exchange.finishReason.c_str(),
                exchange.usedGrammar ? "on" : "off",
                exchange.usedMtp ? "on" : "off");
            if (exchange.usedMtp)
            {
                ImGui::Text("MTP Draft: %d accepted / %d generated",
                    exchange.acceptedDraftTokens,
                    exchange.draftTokens);
            }
            for (const AiActionLogEntry& action : exchange.actions)
            {
                ImGui::BulletText("%s: %s%s",
                    action.method.empty() ? "(unknown)" : action.method.c_str(),
                    action.applied ? "applied" : "rejected",
                    action.diagnostics.empty() ? "" : (" - " + action.diagnostics).c_str());
            }
            if (ImGui::SmallButton("Copy JSON"))
            {
                ImGui::SetClipboardText(exchange.extractedJson.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Raw"))
            {
                ImGui::SetClipboardText(exchange.rawResponse.c_str());
            }
            if (!exchange.extractedJson.empty() && ImGui::TreeNode("Extracted JSON"))
            {
                ImGui::TextWrapped("%s", exchange.extractedJson.c_str());
                ImGui::TreePop();
            }
            if (!exchange.rawResponse.empty() && ImGui::TreeNode("Raw Response"))
            {
                ImGui::TextWrapped("%s", exchange.rawResponse.c_str());
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar(2);
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
    ImGui::TextWrapped("Shadows: %s", stats.shadowStatus.c_str());
    const LocalLlmStatus llmStatus = m_llm.Status();
    ImGui::TextWrapped("LLM Inference: %s", llmStatus.inferenceMode.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("Root: %s", PathToUtf8(m_rootDirectory).c_str());
    if (!m_uiSettingsDiagnostics.empty())
    {
        ImGui::TextWrapped("UI: %s", m_uiSettingsDiagnostics.c_str());
    }
    if (m_uiSettingsRestartRequired)
    {
        ImGui::TextWrapped("UI: restart required for font and style changes.");
    }
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

void ChatLookDevApp::DrawUiSettingsPanel()
{
    if (!m_showUiSettings)
    {
        return;
    }

    ImGui::Begin("UI Settings", &m_showUiSettings);
    rb::UiSettings edited = m_uiSettings;
    bool changed = false;
    changed |= ImGui::SliderFloat("UI Font Size", &edited.uiFontSize, 14.0f, 28.0f, "%.1f px");
    changed |= ImGui::SliderFloat("Chat Font Size", &edited.chatFontSize, 16.0f, 32.0f, "%.1f px");
    changed |= ImGui::SliderFloat("UI Scale", &edited.uiScale, 0.85f, 1.5f, "%.2f");
    changed |= ImGui::SliderFloat("Chat Transcript Ratio", &edited.chatTranscriptHeightRatio, 0.35f, 0.70f, "%.2f");
    changed |= ImGui::Checkbox("Large Frame Padding", &edited.largeFramePadding);

    if (changed)
    {
        const bool restartFieldChanged =
            edited.uiFontSize != m_uiSettings.uiFontSize
            || edited.chatFontSize != m_uiSettings.chatFontSize
            || edited.uiScale != m_uiSettings.uiScale
            || edited.largeFramePadding != m_uiSettings.largeFramePadding;
        m_uiSettings = ClampUiSettings(edited);
        if (SaveUiSettings() && restartFieldChanged)
        {
            m_uiSettingsRestartRequired = true;
        }
    }

    if (!m_uiSettingsPath.empty())
    {
        ImGui::TextWrapped("Settings: %s", PathToUtf8(m_uiSettingsPath).c_str());
    }
    ImGui::TextWrapped("Font size, UI scale, and padding changes are applied on next launch. Chat transcript ratio applies immediately.");
    if (m_uiSettingsRestartRequired)
    {
        ImGui::TextWrapped("Restart ChatLookDev to apply font and style changes.");
    }
    if (!m_uiSettingsDiagnostics.empty())
    {
        ImGui::TextWrapped("%s", m_uiSettingsDiagnostics.c_str());
    }

    if (ImGui::Button("Reset UI Settings"))
    {
        ResetUiSettings();
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
    m_project.lookDevShadowSettings.resolution = NormalizeShadowResolution(m_project.lookDevShadowSettings.resolution);
    m_backend.SetSkyColors(m_project.skyTopColor, m_project.skyHorizonColor);
    m_backend.SetLookDevEnvironment(m_project.lookDevEnvironment);
    m_backend.SetLookDevViewSettings(m_project.lookDevViewSettings);
    m_backend.SetLookDevShadowSettings(m_project.lookDevShadowSettings);
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

void ChatLookDevApp::CommitCameraState(const rb::ViewportCamera& camera)
{
    m_backend.SetCameraState(camera);
    m_project.viewportCamera = m_backend.CameraState();
    m_project.hasViewportCamera = true;
    MarkProjectDirty();
}

void ChatLookDevApp::FrameSceneCamera()
{
    m_backend.ResetCameraToScene();
    m_project.viewportCamera = m_backend.CameraState();
    m_project.hasViewportCamera = true;
    MarkProjectDirty();
}

bool ChatLookDevApp::ApplyCameraPreset(const std::string& presetName, bool frameScene)
{
    const CameraPreset* preset = FindCameraPreset(presetName);
    if (!preset)
    {
        return false;
    }
    if (frameScene)
    {
        m_backend.ResetCameraToScene();
    }
    rb::ViewportCamera camera = m_backend.CameraState();
    camera.yaw = preset->yawDegrees * DegToRad;
    camera.pitch = std::clamp(preset->pitchDegrees * DegToRad, -1.55f, 1.55f);
    CommitCameraState(camera);
    return true;
}

bool ChatLookDevApp::StoreCameraBookmark(std::size_t slot)
{
    if (slot >= m_project.cameraBookmarks.size())
    {
        return false;
    }
    m_project.cameraBookmarks[slot].valid = true;
    m_project.cameraBookmarks[slot].camera = m_backend.CameraState();
    MarkProjectDirty();
    return true;
}

bool ChatLookDevApp::RecallCameraBookmark(std::size_t slot)
{
    if (slot >= m_project.cameraBookmarks.size() || !m_project.cameraBookmarks[slot].valid)
    {
        return false;
    }
    CommitCameraState(m_project.cameraBookmarks[slot].camera);
    return true;
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

void ChatLookDevApp::ResetUiLayout()
{
    if (m_backend.ResetImGuiLayout())
    {
        m_projectDiagnostics = "Reset ImGui layout to default.";
    }
    else
    {
        m_projectDiagnostics = "Failed to reset ImGui layout.";
    }
}

void ChatLookDevApp::LoadUiSettings()
{
    m_uiSettingsPath = (m_rootDirectory / "ui.user.json").lexically_normal();
    m_uiSettings = {};
    m_uiSettingsDiagnostics.clear();
    m_uiSettingsRestartRequired = false;

    if (!PathExists(m_uiSettingsPath))
    {
        return;
    }

    try
    {
        const JsonValue root = JsonParser(TrimAscii(ReadTextFile(m_uiSettingsPath))).Parse();
        if (root.type != JsonValue::Type::Object)
        {
            throw std::runtime_error("UI settings root must be an object.");
        }

        rb::UiSettings defaults;
        rb::UiSettings loaded;
        std::string rangeDiagnostics;
        loaded.uiFontSize = UiSettingNumberOrDefault(root, "uiFontSize", defaults.uiFontSize, 14.0f, 28.0f, rangeDiagnostics);
        loaded.chatFontSize = UiSettingNumberOrDefault(root, "chatFontSize", defaults.chatFontSize, 16.0f, 32.0f, rangeDiagnostics);
        loaded.uiScale = UiSettingNumberOrDefault(root, "uiScale", defaults.uiScale, 0.85f, 1.5f, rangeDiagnostics);
        loaded.chatTranscriptHeightRatio = UiSettingNumberOrDefault(root, "chatTranscriptHeightRatio", defaults.chatTranscriptHeightRatio, 0.35f, 0.70f, rangeDiagnostics);
        loaded.largeFramePadding = JsonBoolOr(root, "largeFramePadding", defaults.largeFramePadding);
        m_uiSettings = ClampUiSettings(loaded);
        m_uiSettingsDiagnostics = rangeDiagnostics;
    }
    catch (const std::exception& ex)
    {
        m_uiSettings = {};
        m_uiSettingsDiagnostics = "UI settings load failed; using defaults: " + std::string(ex.what());
    }
}

bool ChatLookDevApp::SaveUiSettings()
{
    try
    {
        if (m_uiSettingsPath.empty())
        {
            m_uiSettingsPath = (m_rootDirectory / "ui.user.json").lexically_normal();
        }
        m_uiSettings = ClampUiSettings(m_uiSettings);
        std::ostringstream json;
        json << "{\n";
        json << "  \"version\": 1,\n";
        json << "  \"uiFontSize\": " << m_uiSettings.uiFontSize << ",\n";
        json << "  \"chatFontSize\": " << m_uiSettings.chatFontSize << ",\n";
        json << "  \"uiScale\": " << m_uiSettings.uiScale << ",\n";
        json << "  \"chatTranscriptHeightRatio\": " << m_uiSettings.chatTranscriptHeightRatio << ",\n";
        json << "  \"largeFramePadding\": " << (m_uiSettings.largeFramePadding ? "true" : "false") << "\n";
        json << "}\n";
        WriteTextFile(m_uiSettingsPath, json.str());
        m_uiSettingsDiagnostics = "UI settings saved.";
        return true;
    }
    catch (const std::exception& ex)
    {
        m_uiSettingsDiagnostics = "UI settings save failed: " + std::string(ex.what());
        return false;
    }
}

void ChatLookDevApp::ResetUiSettings()
{
    if (m_uiSettingsPath.empty())
    {
        m_uiSettingsPath = (m_rootDirectory / "ui.user.json").lexically_normal();
    }
    std::error_code ec;
    std::filesystem::remove(m_uiSettingsPath, ec);
    m_uiSettings = {};
    m_uiSettingsRestartRequired = true;
    m_uiSettingsDiagnostics = ec ? "Failed to remove UI settings file." : "Reset UI settings to defaults. Restart to apply font and style changes.";
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
        json << "  \"camera\": " << CameraJson(m_project.viewportCamera) << ",\n";
        json << "  \"cameraBookmarks\": [\n";
        for (std::size_t i = 0; i < m_project.cameraBookmarks.size(); ++i)
        {
            const rb::CameraBookmark& bookmark = m_project.cameraBookmarks[i];
            json << "    { \"valid\": " << (bookmark.valid ? "true" : "false")
                 << ", \"camera\": " << CameraJson(bookmark.camera) << " }";
            if (i + 1 < m_project.cameraBookmarks.size())
            {
                json << ",";
            }
            json << "\n";
        }
        json << "  ],\n";
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
        json << "  \"shadow\": { "
             << "\"enabled\": " << (m_project.lookDevShadowSettings.enabled ? "true" : "false") << ", "
             << "\"resolution\": " << NormalizeShadowResolution(m_project.lookDevShadowSettings.resolution) << ", "
             << "\"strength\": " << m_project.lookDevShadowSettings.strength << ", "
             << "\"bias\": " << m_project.lookDevShadowSettings.bias << ", "
             << "\"softness\": " << m_project.lookDevShadowSettings.softness << ", "
             << "\"fitScale\": " << m_project.lookDevShadowSettings.fitScale << " },\n";
        json << "  \"llm\": { "
             << "\"modelPath\": \"" << EscapeJson(ProjectPathString(m_llmConfig.modelPath, projectDirectory)) << "\", "
             << "\"mtpDraftModelPath\": \"" << EscapeJson(ProjectPathString(m_llmConfig.mtpDraftModelPath, projectDirectory)) << "\", "
             << "\"contextTokens\": " << m_llmConfig.contextTokens << ", "
             << "\"maxTokens\": " << m_llmConfig.maxTokens << ", "
             << "\"gpuLayers\": " << m_llmConfig.gpuLayers << ", "
             << "\"threads\": " << m_llmConfig.threads << ", "
             << "\"temperature\": " << m_llmConfig.temperature << ", "
             << "\"topP\": " << m_llmConfig.topP << ", "
             << "\"topK\": " << m_llmConfig.topK << ", "
             << "\"structuredJson\": " << (m_llmConfig.structuredJson ? "true" : "false") << ", "
             << "\"mtpEnabled\": " << (m_llmConfig.mtpEnabled ? "true" : "false") << ", "
             << "\"mtpDraftTokens\": " << m_llmConfig.mtpDraftTokens << ", "
             << "\"mtpMinP\": " << m_llmConfig.mtpMinP << ", "
             << "\"mtpBackendSampling\": " << (m_llmConfig.mtpBackendSampling ? "true" : "false") << " },\n";
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
            loadedProject.viewportCamera.pitch = std::clamp(static_cast<float>(JsonNumberOr(*camera, "pitch", loadedProject.viewportCamera.pitch)), -1.55f, 1.55f);
            loadedProject.viewportCamera.distance = std::max(static_cast<float>(JsonNumberOr(*camera, "distance", loadedProject.viewportCamera.distance)), 0.001f);
            loadedProject.viewportCamera.fovDegrees = std::clamp(static_cast<float>(JsonNumberOr(*camera, "fovDegrees", loadedProject.viewportCamera.fovDegrees)), 5.0f, 120.0f);
            loadedProject.hasViewportCamera = true;
        }

        if (const JsonValue* cameraBookmarks = FindMember(root, "cameraBookmarks"); cameraBookmarks && cameraBookmarks->type == JsonValue::Type::Array)
        {
            const std::size_t count = std::min(cameraBookmarks->array.size(), loadedProject.cameraBookmarks.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const JsonValue& bookmarkValue = cameraBookmarks->array[i];
                if (bookmarkValue.type != JsonValue::Type::Object)
                {
                    continue;
                }
                rb::CameraBookmark bookmark;
                bookmark.valid = JsonBoolOr(bookmarkValue, "valid", false);
                const JsonValue* cameraValue = FindMember(bookmarkValue, "camera");
                if (!cameraValue)
                {
                    cameraValue = &bookmarkValue;
                }
                if (cameraValue->type == JsonValue::Type::Object)
                {
                    bookmark.camera.target = JsonFloat3Or(*cameraValue, "target", bookmark.camera.target);
                    bookmark.camera.yaw = static_cast<float>(JsonNumberOr(*cameraValue, "yaw", bookmark.camera.yaw));
                    bookmark.camera.pitch = std::clamp(static_cast<float>(JsonNumberOr(*cameraValue, "pitch", bookmark.camera.pitch)), -1.55f, 1.55f);
                    bookmark.camera.distance = std::max(static_cast<float>(JsonNumberOr(*cameraValue, "distance", bookmark.camera.distance)), 0.001f);
                    bookmark.camera.fovDegrees = std::clamp(static_cast<float>(JsonNumberOr(*cameraValue, "fovDegrees", bookmark.camera.fovDegrees)), 5.0f, 120.0f);
                }
                loadedProject.cameraBookmarks[i] = bookmark;
            }
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

        if (const JsonValue* shadow = FindMember(root, "shadow"); shadow && shadow->type == JsonValue::Type::Object)
        {
            loadedProject.lookDevShadowSettings.enabled = JsonBoolOr(*shadow, "enabled", loadedProject.lookDevShadowSettings.enabled);
            loadedProject.lookDevShadowSettings.resolution = NormalizeShadowResolution(static_cast<std::uint32_t>(JsonNumberOr(*shadow, "resolution", loadedProject.lookDevShadowSettings.resolution)));
            loadedProject.lookDevShadowSettings.strength = std::clamp(static_cast<float>(JsonNumberOr(*shadow, "strength", loadedProject.lookDevShadowSettings.strength)), 0.0f, 1.0f);
            loadedProject.lookDevShadowSettings.bias = std::clamp(static_cast<float>(JsonNumberOr(*shadow, "bias", loadedProject.lookDevShadowSettings.bias)), 0.0f, 0.05f);
            loadedProject.lookDevShadowSettings.softness = std::clamp(static_cast<float>(JsonNumberOr(*shadow, "softness", loadedProject.lookDevShadowSettings.softness)), 0.0f, 8.0f);
            loadedProject.lookDevShadowSettings.fitScale = std::clamp(static_cast<float>(JsonNumberOr(*shadow, "fitScale", loadedProject.lookDevShadowSettings.fitScale)), 1.0f, 4.0f);
        }

        if (const JsonValue* llm = FindMember(root, "llm"); llm && llm->type == JsonValue::Type::Object)
        {
            loadedLlm.modelPath = ResolveProjectPath(JsonStringOr(*llm, "modelPath"), projectDirectory);
            loadedLlm.mtpDraftModelPath = ResolveProjectPath(JsonStringOr(*llm, "mtpDraftModelPath"), projectDirectory);
            loadedLlm.contextTokens = static_cast<int>(JsonNumberOr(*llm, "contextTokens", loadedLlm.contextTokens));
            loadedLlm.maxTokens = static_cast<int>(JsonNumberOr(*llm, "maxTokens", loadedLlm.maxTokens));
            loadedLlm.gpuLayers = static_cast<int>(JsonNumberOr(*llm, "gpuLayers", loadedLlm.gpuLayers));
            loadedLlm.threads = static_cast<int>(JsonNumberOr(*llm, "threads", loadedLlm.threads));
            loadedLlm.threads = std::max(1, loadedLlm.threads);
            loadedLlm.temperature = static_cast<float>(JsonNumberOr(*llm, "temperature", loadedLlm.temperature));
            loadedLlm.topP = static_cast<float>(JsonNumberOr(*llm, "topP", loadedLlm.topP));
            loadedLlm.topK = static_cast<int>(JsonNumberOr(*llm, "topK", loadedLlm.topK));
            loadedLlm.structuredJson = JsonBoolOr(*llm, "structuredJson", loadedLlm.structuredJson);
            loadedLlm.mtpEnabled = JsonBoolOr(*llm, "mtpEnabled", loadedLlm.mtpEnabled);
            loadedLlm.mtpDraftTokens = std::clamp(static_cast<int>(JsonNumberOr(*llm, "mtpDraftTokens", loadedLlm.mtpDraftTokens)), 1, 16);
            loadedLlm.mtpMinP = std::clamp(static_cast<float>(JsonNumberOr(*llm, "mtpMinP", loadedLlm.mtpMinP)), 0.0f, 1.0f);
            loadedLlm.mtpBackendSampling = JsonBoolOr(*llm, "mtpBackendSampling", loadedLlm.mtpBackendSampling);
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
        m_pendingUserPrompt = prompt;
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
    prompt << "Schema: {\"reply\":\"short user-facing reply\",\"actions\":[{\"method\":\"set_view_settings|set_environment_settings|set_sun_settings|set_shadow_settings|set_material_preview|set_camera|set_model_transform\",\"params\":{}}]}.\n";
    prompt << "Always include an actions array. Use [] when no parameter change is needed.\n";
    prompt << "Allowed model transform params: translation float3 absolute, translationDelta float3 additive, rotationDegrees float3 absolute XYZ degrees, rotationDegreesDelta float3 additive XYZ degrees, rotationRadians float3 absolute, rotationRadiansDelta float3 additive, reset boolean.\n";
    prompt << "Allowed view params: exposure, gamma, toneMapper(None/Reinhard/ACES), displayMode(Beauty/Base Color/Normal/Roughness/Metallic/Ambient Occlusion/Emissive/Lighting Only/Shadow Mask), turntableEnabled, turntableSpeed.\n";
    prompt << "Allowed environment params: rotationYaw, intensity, backgroundMode(Sky Color/HDRI/Checker), skyTopColor, skyHorizonColor.\n";
    prompt << "Allowed sun params: sunDirection float3, sunColor float3, illuminanceLux.\n";
    prompt << "Allowed shadow params: enabled boolean, resolution 1024/2048/4096, strength 0..1, bias 0..0.05, softness 0..8, fitScale 1..4.\n";
    prompt << "Allowed material params: materialName optional, baseColorFactor float4, emissiveFactor float4, roughnessFactor, metallicFactor, normalStrength, occlusionStrength, alphaMode(Opaque/Mask/Blend), alphaCutoff, packedOcclusionRoughnessMetallic, flipNormalGreen.\n";
    prompt << "Allowed camera params: target float3, yaw/pitch radians, yawDegrees/pitchDegrees optional, distance, fovDegrees 5..120, frameScene boolean, preset(Front/Back/Left/Right/Top/Bottom/Iso), storeBookmark 1..3, recallBookmark 1..3.\n";
    prompt << "Examples:\n";
    prompt << "User: 露出を下げて -> {\"reply\":\"露出を下げました。\",\"actions\":[{\"method\":\"set_view_settings\",\"params\":{\"exposure\":-1.0}}]}\n";
    prompt << "User: 太陽を強くして -> {\"reply\":\"太陽光を強くしました。\",\"actions\":[{\"method\":\"set_sun_settings\",\"params\":{\"illuminanceLux\":50000}}]}\n";
    prompt << "User: 影を柔らかくして -> {\"reply\":\"影を柔らかくしました。\",\"actions\":[{\"method\":\"set_shadow_settings\",\"params\":{\"softness\":4.0}}]}\n";
    prompt << "User: シャドウマスクを表示して -> {\"reply\":\"シャドウマスク表示に切り替えました。\",\"actions\":[{\"method\":\"set_view_settings\",\"params\":{\"displayMode\":\"Shadow Mask\"}}]}\n";
    prompt << "User: 正面から見せて -> {\"reply\":\"正面ビューに切り替えました。\",\"actions\":[{\"method\":\"set_camera\",\"params\":{\"preset\":\"Front\",\"frameScene\":true}}]}\n";
    prompt << "User: 今のカメラを1番に保存して -> {\"reply\":\"カメラを1番に保存しました。\",\"actions\":[{\"method\":\"set_camera\",\"params\":{\"storeBookmark\":1}}]}\n";
    prompt << "User: 1番のカメラに戻して -> {\"reply\":\"1番のカメラに戻しました。\",\"actions\":[{\"method\":\"set_camera\",\"params\":{\"recallBookmark\":1}}]}\n";
    prompt << "User: モデルを右へ動かして -> {\"reply\":\"モデルを右へ少し移動しました。\",\"actions\":[{\"method\":\"set_model_transform\",\"params\":{\"translationDelta\":[0.25,0,0]}}]}\n";
    prompt << "User: この material を粗くして -> {\"reply\":\"選択中のマテリアルを粗くしました。\",\"actions\":[{\"method\":\"set_material_preview\",\"params\":{\"roughnessFactor\":0.85}}]}\n";
    prompt << "Do not request shader edits, MCP, automation, runtime compilation, DXR, path tracing, or external tools.\n";
    prompt << "Current state JSON: " << BuildControlStateJson();
    return prompt.str();
}

std::string ChatLookDevApp::BuildControlStateJson() const
{
    std::ostringstream json;
    const std::size_t materialCount = m_project.materialAssignments.size();
    const std::size_t selectedMaterialIndex = materialCount == 0 ? 0 : std::min(m_selectedMaterial, materialCount - 1);
    const std::string selectedMaterial = materialCount == 0 ? "" : m_project.materialAssignments[selectedMaterialIndex].materialName;
    const rb::ViewportCamera camera = m_backend.CameraState();
    json << "{";
    json << "\"scenePath\":\"" << EscapeJson(PathToUtf8(m_project.scenePath)) << "\",";
    json << "\"selectedMaterial\":\"" << EscapeJson(selectedMaterial) << "\",";
    json << "\"materialCount\":" << materialCount << ",";
    json << "\"modelTransform\":{\"translation\":" << Float3Json(m_project.modelTransform.translation) << ",\"rotationDegrees\":" << Float3Json(m_project.modelTransform.rotationDegrees) << "},";
    json << "\"camera\":{\"target\":" << Float3Json(camera.target) << ",\"yaw\":" << camera.yaw << ",\"pitch\":" << camera.pitch << ",\"distance\":" << camera.distance << ",\"fovDegrees\":" << camera.fovDegrees << "},";
    json << "\"cameraBookmarks\":[";
    for (std::size_t i = 0; i < m_project.cameraBookmarks.size(); ++i)
    {
        if (i > 0)
        {
            json << ",";
        }
        json << "{\"slot\":" << (i + 1) << ",\"valid\":" << (m_project.cameraBookmarks[i].valid ? "true" : "false") << "}";
    }
    json << "],";
    json << "\"environment\":{\"hdriPath\":\"" << EscapeJson(PathToUtf8(m_project.lookDevEnvironment.environmentPath)) << "\",\"rotationYaw\":" << m_project.lookDevEnvironment.rotationYaw << ",\"intensity\":" << m_project.lookDevEnvironment.intensity << ",\"backgroundMode\":\"" << BackgroundModeName(m_project.lookDevEnvironment.backgroundMode) << "\",\"sunDirection\":" << Float3Json(m_project.lookDevEnvironment.sunDirection) << ",\"sunColor\":" << Float3Json(m_project.lookDevEnvironment.sunColor) << ",\"illuminanceLux\":" << m_project.lookDevEnvironment.sunIntensity << "},";
    json << "\"view\":{\"exposure\":" << m_project.lookDevViewSettings.exposure << ",\"gamma\":" << m_project.lookDevViewSettings.gamma << ",\"toneMapper\":\"" << ToneMapperName(m_project.lookDevViewSettings.toneMapper) << "\",\"displayMode\":\"" << DisplayModeName(m_project.lookDevViewSettings.displayMode) << "\"},";
    json << "\"shadow\":{\"enabled\":" << (m_project.lookDevShadowSettings.enabled ? "true" : "false") << ",\"resolution\":" << NormalizeShadowResolution(m_project.lookDevShadowSettings.resolution) << ",\"strength\":" << m_project.lookDevShadowSettings.strength << ",\"bias\":" << m_project.lookDevShadowSettings.bias << ",\"softness\":" << m_project.lookDevShadowSettings.softness << ",\"fitScale\":" << m_project.lookDevShadowSettings.fitScale << "},";
    json << "\"materialsPreview\":[";
    std::vector<std::size_t> materialPreviewIndices;
    materialPreviewIndices.reserve(std::min(materialCount, MaxPromptMaterialPreviewCount));
    if (materialCount > 0)
    {
        materialPreviewIndices.push_back(selectedMaterialIndex);
    }
    for (std::size_t i = 0; i < materialCount && materialPreviewIndices.size() < MaxPromptMaterialPreviewCount; ++i)
    {
        if (std::find(materialPreviewIndices.begin(), materialPreviewIndices.end(), i) == materialPreviewIndices.end())
        {
            materialPreviewIndices.push_back(i);
        }
    }
    for (std::size_t previewIndex = 0; previewIndex < materialPreviewIndices.size(); ++previewIndex)
    {
        if (previewIndex > 0)
        {
            json << ",";
        }
        const std::size_t i = materialPreviewIndices[previewIndex];
        const rb::MaterialAssignment& material = m_project.materialAssignments[i];
        json << "{\"index\":" << i
             << ",\"selected\":" << (i == selectedMaterialIndex ? "true" : "false")
             << ",\"name\":\"" << EscapeJson(material.materialName)
             << "\",\"roughnessFactor\":" << material.roughnessFactor
             << ",\"metallicFactor\":" << material.metallicFactor
             << ",\"baseColorFactor\":" << Float4Json(material.baseColorFactor) << "}";
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
            HandleLlmResponse(event);
        }
        else if (event.kind == LocalLlmEvent::Kind::Error)
        {
            m_chatMessages.push_back({ "system", event.text });
            if (!m_pendingUserPrompt.empty())
            {
                AiExchange exchange;
                exchange.userPrompt = m_pendingUserPrompt;
                exchange.rejectedReason = event.text;
                exchange.diagnostics = "LLM generation error.";
                m_aiHistory.push_back(std::move(exchange));
                m_pendingUserPrompt.clear();
            }
            m_scrollChatToBottom = true;
        }
        else
        {
            m_lastActionDiagnostics = event.text;
            if (event.text == "Generation stopped." && !m_pendingUserPrompt.empty())
            {
                AiExchange exchange;
                exchange.userPrompt = m_pendingUserPrompt;
                exchange.rejectedReason = "Generation stopped.";
                exchange.diagnostics = "Generation was cancelled before a response was applied.";
                m_aiHistory.push_back(std::move(exchange));
                m_pendingUserPrompt.clear();
                m_scrollChatToBottom = true;
            }
        }
    }
}

void ChatLookDevApp::HandleLlmResponse(const LocalLlmEvent& event)
{
    AiExchange exchange;
    exchange.userPrompt = m_pendingUserPrompt;
    exchange.rawResponse = event.rawText.empty() ? event.text : event.rawText;
    exchange.promptTokens = event.promptTokens;
    exchange.outputTokens = event.outputTokens;
    exchange.elapsedMs = event.elapsedMs;
    exchange.finishReason = event.finishReason;
    exchange.usedGrammar = event.usedGrammar;
    exchange.usedMtp = event.usedMtp;
    exchange.draftTokens = event.draftTokens;
    exchange.acceptedDraftTokens = event.acceptedDraftTokens;
    exchange.diagnostics = event.diagnostics;
    m_lastLlmStats = "Last generation: prompt " + std::to_string(event.promptTokens)
        + " tokens, output " + std::to_string(event.outputTokens)
        + " tokens, " + event.finishReason
        + ", " + std::to_string(static_cast<int>(event.elapsedMs)) + " ms"
        + (event.usedGrammar ? ", grammar on." : ", grammar off.");
    if (event.usedMtp)
    {
        m_lastLlmStats += " MTP " + std::to_string(event.acceptedDraftTokens)
            + "/" + std::to_string(event.draftTokens) + " accepted.";
    }

    try
    {
        const std::string trimmed = TrimAscii(exchange.rawResponse);
        std::string jsonText;
        if (!ExtractFirstJsonObject(trimmed, jsonText))
        {
            throw std::runtime_error("AI response did not contain a JSON object.");
        }

        const bool recoveredFromWrappedOutput = jsonText.size() != trimmed.size();
        exchange.extractedJson = jsonText;
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
        exchange.assistantReply = replyValue->string;

        std::ostringstream actionDiagnostics;
        struct PendingAction
        {
            std::string method;
            const JsonValue* params = nullptr;
        };
        std::vector<PendingAction> pendingActions;
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
                    exchange.actions.push_back({ {}, false, "Rejected non-object action." });
                    throw std::runtime_error("Rejected non-object action.");
                }
                const std::string method = JsonStringOr(action, "method");
                if (!IsSupportedAiAction(method))
                {
                    const std::string diagnostics = "Unsupported action method: " + method + ".";
                    exchange.actions.push_back({ method, false, diagnostics });
                    throw std::runtime_error(diagnostics);
                }
                const JsonValue* params = FindMember(action, "params");
                if (!params)
                {
                    const std::string diagnostics = "Rejected action with missing params.";
                    exchange.actions.push_back({ method, false, diagnostics });
                    throw std::runtime_error(diagnostics);
                }
                if (params->type != JsonValue::Type::Object)
                {
                    const std::string diagnostics = "Rejected action with non-object params.";
                    exchange.actions.push_back({ method, false, diagnostics });
                    throw std::runtime_error(diagnostics);
                }
                std::string diagnostics;
                if (!ApplyAiAction(method, *params, diagnostics, false))
                {
                    exchange.actions.push_back({ method, false, diagnostics });
                    throw std::runtime_error(diagnostics);
                }
                pendingActions.push_back({ method, params });
            }
        }

        for (const PendingAction& action : pendingActions)
        {
            std::string diagnostics;
            if (ApplyAiAction(action.method, *action.params, diagnostics, true))
            {
                ++exchange.appliedCount;
                exchange.actions.push_back({ action.method, true, diagnostics.empty() ? "Applied." : diagnostics });
            }
            else
            {
                ++exchange.rejectedCount;
                exchange.actions.push_back({ action.method, false, diagnostics });
                actionDiagnostics << diagnostics << " ";
            }
        }

        m_chatMessages.push_back({ "assistant", replyValue->string });
        actionDiagnostics << "Applied actions: " << exchange.appliedCount << ".";
        if (recoveredFromWrappedOutput)
        {
            actionDiagnostics << " Ignored non-JSON text around the response.";
        }
        if (!event.diagnostics.empty())
        {
            actionDiagnostics << " " << event.diagnostics;
        }
        m_lastActionDiagnostics = actionDiagnostics.str();
        exchange.diagnostics = m_lastActionDiagnostics;
        m_aiHistory.push_back(exchange);
        m_pendingUserPrompt.clear();
        m_scrollChatToBottom = true;
    }
    catch (const std::exception& ex)
    {
        m_chatMessages.push_back({ "system", "Rejected invalid AI JSON: " + std::string(ex.what()) });
        exchange.rejectedReason = ex.what();
        exchange.rejectedCount = static_cast<int>(std::max<std::size_t>(exchange.actions.size(), 1));
        m_lastActionDiagnostics = "Rejected invalid AI JSON.";
        exchange.diagnostics = m_lastActionDiagnostics;
        m_aiHistory.push_back(exchange);
        m_pendingUserPrompt.clear();
        m_scrollChatToBottom = true;
    }
}

bool ChatLookDevApp::ApplyAiAction(const std::string& method, const JsonValue& params, std::string& diagnostics, bool commit)
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

        if (commit)
        {
            m_project.modelTransform = transform;
            ApplyModelTransform();
            MarkProjectDirty();
        }
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
        if (commit)
        {
            m_project.lookDevViewSettings = view;
            ApplyLookDevSettings();
            MarkProjectDirty();
        }
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
        if (commit)
        {
            m_project.lookDevEnvironment = environment;
            m_project.skyTopColor = skyTop;
            m_project.skyHorizonColor = skyHorizon;
            ApplyLookDevSettings();
            MarkProjectDirty();
        }
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
        if (commit)
        {
            m_project.lookDevEnvironment = environment;
            ApplyLookDevSettings();
            MarkProjectDirty();
        }
        return true;
    }

    if (method == "set_shadow_settings")
    {
        rb::LookDevShadowSettings shadow = m_project.lookDevShadowSettings;
        // Validate into a local copy first. If any field is invalid, the caller
        // receives a rejection and the visible scene state remains unchanged.
        if (!ReadOptionalBool(params, "enabled", shadow.enabled, diagnostics)) return false;
        float resolution = static_cast<float>(shadow.resolution);
        if (!ReadOptionalNumber(params, "resolution", 1024.0f, 4096.0f, resolution, diagnostics)) return false;
        shadow.resolution = NormalizeShadowResolution(static_cast<std::uint32_t>(std::lround(resolution)));
        if (!ReadOptionalNumber(params, "strength", 0.0f, 1.0f, shadow.strength, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "bias", 0.0f, 0.05f, shadow.bias, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "softness", 0.0f, 8.0f, shadow.softness, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "fitScale", 1.0f, 4.0f, shadow.fitScale, diagnostics)) return false;
        if (commit)
        {
            m_project.lookDevShadowSettings = shadow;
            ApplyLookDevSettings();
            MarkProjectDirty();
        }
        return true;
    }

    if (method == "set_camera")
    {
        rb::ViewportCamera camera = m_backend.CameraState();
        bool frameScene = false;
        if (!ReadOptionalBool(params, "frameScene", frameScene, diagnostics)) return false;
        int recallBookmark = 0;
        if (!ReadOptionalBookmarkSlot(params, "recallBookmark", recallBookmark, diagnostics)) return false;
        int storeBookmark = 0;
        if (!ReadOptionalBookmarkSlot(params, "storeBookmark", storeBookmark, diagnostics)) return false;

        const CameraPreset* preset = nullptr;
        if (const JsonValue* presetValue = FindMember(params, "preset"))
        {
            if (presetValue->type != JsonValue::Type::String)
            {
                diagnostics = "preset must be a string.";
                return false;
            }
            preset = FindCameraPreset(presetValue->string);
            if (!preset)
            {
                diagnostics = "Unknown camera preset.";
                return false;
            }
        }
        if (recallBookmark > 0 && !m_project.cameraBookmarks[static_cast<std::size_t>(recallBookmark - 1)].valid)
        {
            diagnostics = "Camera bookmark is empty.";
            return false;
        }

        if (commit)
        {
            if (frameScene)
            {
                m_backend.ResetCameraToScene();
                camera = m_backend.CameraState();
            }
            if (recallBookmark > 0)
            {
                camera = m_project.cameraBookmarks[static_cast<std::size_t>(recallBookmark - 1)].camera;
            }
        }
        else if (recallBookmark > 0)
        {
            camera = m_project.cameraBookmarks[static_cast<std::size_t>(recallBookmark - 1)].camera;
        }
        if (preset)
        {
            camera.yaw = preset->yawDegrees * DegToRad;
            camera.pitch = std::clamp(preset->pitchDegrees * DegToRad, -1.55f, 1.55f);
        }
        if (!ReadOptionalFloat3(params, "target", -1000000.0f, 1000000.0f, camera.target, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "yaw", -1000.0f, 1000.0f, camera.yaw, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "pitch", -1.55f, 1.55f, camera.pitch, diagnostics)) return false;
        float yawDegrees = camera.yaw * RadToDeg;
        if (!ReadOptionalNumber(params, "yawDegrees", -36000.0f, 36000.0f, yawDegrees, diagnostics)) return false;
        camera.yaw = yawDegrees * DegToRad;
        float pitchDegrees = camera.pitch * RadToDeg;
        if (!ReadOptionalNumber(params, "pitchDegrees", -88.81f, 88.81f, pitchDegrees, diagnostics)) return false;
        camera.pitch = std::clamp(pitchDegrees * DegToRad, -1.55f, 1.55f);
        if (!ReadOptionalNumber(params, "distance", 0.001f, 10000000.0f, camera.distance, diagnostics)) return false;
        if (!ReadOptionalNumber(params, "fovDegrees", 5.0f, 120.0f, camera.fovDegrees, diagnostics)) return false;
        if (commit)
        {
            CommitCameraState(camera);
            if (storeBookmark > 0)
            {
                StoreCameraBookmark(static_cast<std::size_t>(storeBookmark - 1));
            }
            std::ostringstream message;
            message << "Camera updated.";
            if (storeBookmark > 0)
            {
                message << " Stored bookmark " << storeBookmark << ".";
            }
            diagnostics = message.str();
        }
        return true;
    }

    if (method == "set_material_preview")
    {
        if (m_project.materialAssignments.empty())
        {
            diagnostics = "No material is available.";
            return false;
        }
        std::string materialName = JsonStringOr(params, "materialName");
        const std::size_t selectedMaterial = std::min(m_selectedMaterial, m_project.materialAssignments.size() - 1);
        if (materialName.empty())
        {
            materialName = m_project.materialAssignments[selectedMaterial].materialName;
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
        if (commit)
        {
            *materialIt = material;
            m_selectedMaterial = static_cast<std::size_t>(std::distance(m_project.materialAssignments.begin(), materialIt));
            ApplyMaterialAssignments();
            MarkProjectDirty();
        }
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
