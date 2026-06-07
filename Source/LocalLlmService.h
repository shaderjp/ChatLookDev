#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_sampler;

namespace cld
{
struct LocalLlmMessage
{
    std::string role;
    std::string content;
};

struct LocalLlmConfig
{
    std::filesystem::path modelPath;
    int contextTokens = 4096;
    int maxTokens = 512;
    int gpuLayers = 1;
    int threads = 1;
    float temperature = 0.7f;
    float topP = 0.9f;
    int topK = 40;
    bool structuredJson = true;
};

struct LocalLlmEvent
{
    enum class Kind
    {
        Status,
        Response,
        Error
    };
    Kind kind = Kind::Status;
    std::string text;
    std::string rawText;
    int promptTokens = 0;
    int outputTokens = 0;
    double elapsedMs = 0.0;
    std::string finishReason;
    bool usedGrammar = false;
    std::string diagnostics;
};

enum class LocalLlmState
{
    Stopped,
    Loading,
    Ready,
    Generating,
    Failed
};

struct LocalLlmStatus
{
    LocalLlmState state = LocalLlmState::Stopped;
    std::string stateText = "Stopped";
    std::string lastError;
    std::filesystem::path modelPath;
    int contextTokens = 0;
    int gpuLayers = 1;
    std::string inferenceMode = "CPU";
    std::string inferenceDevice;
    bool gpuOffloadAvailable = false;
};

class LocalLlmService
{
public:
    LocalLlmService();
    ~LocalLlmService();

    LocalLlmService(const LocalLlmService&) = delete;
    LocalLlmService& operator=(const LocalLlmService&) = delete;

    bool LoadAsync(const LocalLlmConfig& config);
    void Stop();
    void CancelGeneration();
    bool Submit(const std::vector<LocalLlmMessage>& messages);
    std::vector<LocalLlmEvent> DrainEvents();
    LocalLlmStatus Status() const;

private:
    struct GenerationResult
    {
        std::string text;
        int promptTokens = 0;
        int outputTokens = 0;
        double elapsedMs = 0.0;
        std::string finishReason;
        bool usedGrammar = false;
        std::string diagnostics;
    };

    void LoadWorker(LocalLlmConfig config);
    void GenerateWorker(std::vector<LocalLlmMessage> messages);
    GenerationResult Generate(const std::vector<LocalLlmMessage>& messages);
    std::vector<int> Tokenize(const std::string& text, bool addSpecial, bool parseSpecial) const;
    std::string TokenToPiece(int token) const;
    std::string ApplyChatTemplate(const std::vector<LocalLlmMessage>& messages) const;
    void PushEvent(LocalLlmEvent::Kind kind, const std::string& text);
    void PushResponseEvent(const GenerationResult& result);
    void SetState(LocalLlmState state, const std::string& text);
    void SetError(const std::string& error);
    void JoinWorker();
    void ReleaseModel();
    llama_sampler* CreateSamplerChain(llama_model* model, const LocalLlmConfig& config, std::string& diagnostics, bool& usedGrammar) const;

    mutable std::mutex m_mutex;
    std::deque<LocalLlmEvent> m_events;
    LocalLlmConfig m_config;
    LocalLlmState m_state = LocalLlmState::Stopped;
    std::string m_stateText = "Stopped";
    std::string m_lastError;
    bool m_structuredGrammarActive = false;
    std::string m_samplerDiagnostics;

    llama_model* m_model = nullptr;
    llama_context* m_context = nullptr;
    std::thread m_worker;
    std::atomic<bool> m_stopRequested = false;
};
}
