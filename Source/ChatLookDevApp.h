#pragma once

#include "D3D12Backend.h"
#include "LocalLlmService.h"
#include "SceneImporter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

struct ImVec2;

namespace cld
{
struct ChatMessage
{
    std::string role;
    std::string text;
};

struct AiActionLogEntry
{
    std::string method;
    bool applied = false;
    std::string diagnostics;
};

struct AiExchange
{
    std::string userPrompt;
    std::string assistantReply;
    std::string rawResponse;
    std::string extractedJson;
    std::string diagnostics;
    std::string rejectedReason;
    std::string finishReason;
    bool usedGrammar = false;
    int promptTokens = 0;
    int outputTokens = 0;
    double elapsedMs = 0.0;
    int appliedCount = 0;
    int rejectedCount = 0;
    std::vector<AiActionLogEntry> actions;
};

class ChatLookDevApp
{
public:
    ChatLookDevApp() = default;
    ~ChatLookDevApp();

    ChatLookDevApp(const ChatLookDevApp&) = delete;
    ChatLookDevApp& operator=(const ChatLookDevApp&) = delete;

    int Run(HINSTANCE instance, int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void Initialize(HINSTANCE instance, int showCommand);
    void Shutdown();
    void MainLoop();
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void InitializeDefaults();
    void BeginFrame();
    void DrawUi();
    void DrawMainMenu();
    void DrawDockSpace();
    void DrawViewportPanel();
    void DrawScenePanel();
    void DrawMaterialPanel();
    void DrawLightingPanel();
    void DrawAiChatPanel();
    void DrawActionHistoryPanel();
    void DrawDiagnosticsPanel();
    void RenderFrame(float deltaSeconds);
    void DrawSunDirectionOverlay(const ImVec2& imageMin, const ImVec2& imageMax);
    void HandleViewportInput(const ImVec2& imageMin, const ImVec2& imageMax);

    std::filesystem::path OpenFileDialog(const wchar_t* filter) const;
    std::filesystem::path SaveFileDialog(const wchar_t* filter, const wchar_t* defaultExtension, const wchar_t* defaultName) const;
    bool LoadScenePath(const std::filesystem::path& path);
    bool LoadEnvironmentPath(const std::filesystem::path& path);
    void UsePreviewScene();
    void ApplyModelTransform();
    void ApplyLookDevSettings();
    void ApplyMaterialAssignments();
    void ApplyImportedTextureOverrides(rb::ImportedScene& scene);
    void EnsureMaterialSelection();
    void MarkProjectDirty();
    void ResetUiLayout();

    void SaveProject();
    void SaveProjectAs();
    bool SaveProjectToDisk(const std::filesystem::path& path);
    void LoadProject();
    bool LoadProjectFromDisk(const std::filesystem::path& path);

    void DrainLlmEvents();
    void LoadLocalModel();
    void SendChatPrompt();
    std::vector<LocalLlmMessage> BuildLlmMessages(const std::string& userText) const;
    std::string BuildSystemPrompt() const;
    std::string BuildControlStateJson() const;
    void HandleLlmResponse(const LocalLlmEvent& event);
    bool ApplyAiAction(const std::string& method, const struct JsonValue& params, std::string& diagnostics, bool commit);

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    bool m_running = false;
    bool m_initialized = false;
    std::filesystem::path m_rootDirectory;

    D3D12Backend m_backend;
    rb::SceneImporter m_sceneImporter;
    LocalLlmService m_llm;
    LocalLlmConfig m_llmConfig;

    rb::ProjectFile m_project;
    rb::ImportedScene m_importedScene;
    std::size_t m_selectedMaterial = 0;
    bool m_projectDirty = false;
    std::string m_sceneDiagnostics = "Using built-in preview cube.";
    std::string m_lastActionDiagnostics;
    std::string m_projectDiagnostics;

    std::vector<ChatMessage> m_chatMessages;
    std::vector<AiExchange> m_aiHistory;
    std::string m_pendingUserPrompt;
    std::string m_lastLlmStats;
    std::array<char, 4096> m_chatInput = {};
    bool m_scrollChatToBottom = false;

    std::chrono::steady_clock::time_point m_lastFrameTime;
};
}
