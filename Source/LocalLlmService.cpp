#include "LocalLlmService.h"

#include <llama.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace
{
std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.c_str()), text.size());
}

std::string StateName(cld::LocalLlmState state)
{
    switch (state)
    {
    case cld::LocalLlmState::Loading: return "Loading";
    case cld::LocalLlmState::Ready: return "Ready";
    case cld::LocalLlmState::Generating: return "Generating";
    case cld::LocalLlmState::Failed: return "Failed";
    case cld::LocalLlmState::Stopped:
    default: return "Stopped";
    }
}

void FillBatch(llama_batch& batch, const std::vector<int>& tokens, int startPos, bool logitsLast)
{
    batch.n_tokens = static_cast<int32_t>(tokens.size());
    for (int32_t i = 0; i < batch.n_tokens; ++i)
    {
        batch.token[i] = tokens[static_cast<std::size_t>(i)];
        batch.pos[i] = startPos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = logitsLast && i == batch.n_tokens - 1;
    }
}

const char* LookDevJsonGrammar()
{
    return R"gbnf(
root ::= ws "{" ws reply-field ws "," ws actions-field ws "}" ws
reply-field ::= "\"reply\"" ws ":" ws string
actions-field ::= "\"actions\"" ws ":" ws "[" ws (action (ws "," ws action)*)? ws "]"
action ::= "{" ws "\"method\"" ws ":" ws method ws "," ws "\"params\"" ws ":" ws object ws "}"
method ::= "\"set_view_settings\"" | "\"set_environment_settings\"" | "\"set_sun_settings\"" | "\"set_material_preview\"" | "\"set_camera\"" | "\"set_model_transform\""
object ::= "{" ws (pair (ws "," ws pair)*)? ws "}"
pair ::= string ws ":" ws value
array ::= "[" ws (value (ws "," ws value)*)? ws "]"
value ::= object | array | string | number | "true" | "false" | "null"
string ::= "\"" ([^"\\] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]))* "\""
number ::= "-"? ("0" | [1-9] [0-9]*) ("." [0-9]+)? ([eE] [-+]? [0-9]+)?
ws ::= [ \t\n\r]*
)gbnf";
}
}

namespace cld
{
LocalLlmService::LocalLlmService()
{
    llama_backend_init();
}

LocalLlmService::~LocalLlmService()
{
    Stop();
    llama_backend_free();
}

bool LocalLlmService::LoadAsync(const LocalLlmConfig& config)
{
    Stop();
    if (config.modelPath.empty() || !std::filesystem::exists(config.modelPath))
    {
        SetError("GGUF model file was not found.");
        SetState(LocalLlmState::Failed, "Failed");
        return false;
    }
    m_stopRequested = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        m_lastError.clear();
    }
    m_worker = std::thread(&LocalLlmService::LoadWorker, this, config);
    return true;
}

void LocalLlmService::Stop()
{
    m_stopRequested = true;
    JoinWorker();
    ReleaseModel();
    SetState(LocalLlmState::Stopped, "Stopped");
}

void LocalLlmService::CancelGeneration()
{
    m_stopRequested = true;
    JoinWorker();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_model && m_context)
    {
        m_state = LocalLlmState::Ready;
        m_stateText = "Ready";
        m_lastError.clear();
    }
}

void LocalLlmService::JoinWorker()
{
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void LocalLlmService::ReleaseModel()
{
    if (m_sampler)
    {
        llama_sampler_free(m_sampler);
        m_sampler = nullptr;
    }
    if (m_context)
    {
        llama_free(m_context);
        m_context = nullptr;
    }
    if (m_model)
    {
        llama_model_free(m_model);
        m_model = nullptr;
    }
    m_structuredGrammarActive = false;
    m_samplerDiagnostics.clear();
}

void LocalLlmService::LoadWorker(LocalLlmConfig config)
{
    try
    {
        SetState(LocalLlmState::Loading, "Loading");
        PushEvent(LocalLlmEvent::Kind::Status, "Loading GGUF model...");

        llama_model_params modelParams = llama_model_default_params();
        modelParams.n_gpu_layers = std::max(0, config.gpuLayers);
        const std::string modelPath = PathToUtf8(config.modelPath);
        llama_model* model = llama_model_load_from_file(modelPath.c_str(), modelParams);
        if (!model)
        {
            throw std::runtime_error("llama_model_load_from_file failed.");
        }

        llama_context_params contextParams = llama_context_default_params();
        contextParams.n_ctx = std::max(1024, config.contextTokens);
        contextParams.n_batch = std::min<int32_t>(2048, contextParams.n_ctx);
        contextParams.n_ubatch = std::min<int32_t>(512, contextParams.n_batch);
        llama_context* context = llama_init_from_model(model, contextParams);
        if (!context)
        {
            llama_model_free(model);
            throw std::runtime_error("llama_init_from_model failed.");
        }
        if (config.threads > 0)
        {
            llama_set_n_threads(context, config.threads, config.threads);
        }

        std::string samplerDiagnostics;
        bool usedGrammar = false;
        llama_sampler* sampler = CreateSamplerChain(model, config, samplerDiagnostics, usedGrammar);
        if (!sampler)
        {
            llama_free(context);
            llama_model_free(model);
            throw std::runtime_error("llama sampler initialization failed.");
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ReleaseModel();
            m_model = model;
            m_context = context;
            m_sampler = sampler;
            m_config = config;
            m_structuredGrammarActive = usedGrammar;
            m_samplerDiagnostics = samplerDiagnostics;
            m_lastError.clear();
        }
        SetState(LocalLlmState::Ready, "Ready");
        PushEvent(LocalLlmEvent::Kind::Status, "Local model ready.");
        if (!samplerDiagnostics.empty())
        {
            PushEvent(LocalLlmEvent::Kind::Status, samplerDiagnostics);
        }
    }
    catch (const std::exception& ex)
    {
        SetError(ex.what());
        SetState(LocalLlmState::Failed, "Failed");
        PushEvent(LocalLlmEvent::Kind::Error, ex.what());
    }
}

bool LocalLlmService::Submit(const std::vector<LocalLlmMessage>& messages)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != LocalLlmState::Ready)
        {
            m_lastError = "Local model is not ready.";
            return false;
        }
    }
    JoinWorker();
    m_stopRequested = false;
    m_worker = std::thread(&LocalLlmService::GenerateWorker, this, messages);
    return true;
}

void LocalLlmService::GenerateWorker(std::vector<LocalLlmMessage> messages)
{
    try
    {
        SetState(LocalLlmState::Generating, "Generating");
        PushEvent(LocalLlmEvent::Kind::Status, "Generating response...");
        const GenerationResult response = Generate(messages);
        if (response.finishReason == "stopped")
        {
            PushEvent(LocalLlmEvent::Kind::Status, "Generation stopped.");
            SetState(LocalLlmState::Ready, "Ready");
            return;
        }
        PushResponseEvent(response);
        SetState(LocalLlmState::Ready, "Ready");
    }
    catch (const std::exception& ex)
    {
        SetError(ex.what());
        PushEvent(LocalLlmEvent::Kind::Error, ex.what());
        SetState(LocalLlmState::Ready, "Ready");
    }
}

std::string LocalLlmService::ApplyChatTemplate(const std::vector<LocalLlmMessage>& messages) const
{
    std::vector<llama_chat_message> chat;
    chat.reserve(messages.size());
    for (const LocalLlmMessage& message : messages)
    {
        chat.push_back({ message.role.c_str(), message.content.c_str() });
    }
    const char* tmpl = llama_model_chat_template(m_model, nullptr);
    int32_t size = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, nullptr, 0);
    if (size <= 0)
    {
        std::ostringstream fallback;
        for (const LocalLlmMessage& message : messages)
        {
            fallback << "<start_of_turn>" << message.role << "\n" << message.content << "<end_of_turn>\n";
        }
        fallback << "<start_of_turn>model\n";
        return fallback.str();
    }
    std::string prompt(static_cast<std::size_t>(size), '\0');
    const int32_t written = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, prompt.data(), size);
    if (written < 0)
    {
        throw std::runtime_error("llama_chat_apply_template failed.");
    }
    prompt.resize(static_cast<std::size_t>(written));
    return prompt;
}

std::vector<int> LocalLlmService::Tokenize(const std::string& text, bool addSpecial, bool parseSpecial) const
{
    const llama_vocab* vocab = llama_model_get_vocab(m_model);
    int32_t count = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), nullptr, 0, addSpecial, parseSpecial);
    if (count == INT32_MIN)
    {
        throw std::runtime_error("Tokenization overflow.");
    }
    if (count < 0)
    {
        count = -count;
    }
    std::vector<int> tokens(static_cast<std::size_t>(count));
    const int32_t written = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()), tokens.data(), count, addSpecial, parseSpecial);
    if (written < 0)
    {
        throw std::runtime_error("Tokenization failed.");
    }
    tokens.resize(static_cast<std::size_t>(written));
    return tokens;
}

std::string LocalLlmService::TokenToPiece(int token) const
{
    const llama_vocab* vocab = llama_model_get_vocab(m_model);
    char buffer[256] = {};
    int32_t size = llama_token_to_piece(vocab, token, buffer, static_cast<int32_t>(sizeof(buffer)), 0, false);
    if (size < 0)
    {
        std::string dynamicBuffer(static_cast<std::size_t>(-size), '\0');
        size = llama_token_to_piece(vocab, token, dynamicBuffer.data(), static_cast<int32_t>(dynamicBuffer.size()), 0, false);
        if (size > 0)
        {
            dynamicBuffer.resize(static_cast<std::size_t>(size));
            return dynamicBuffer;
        }
        return {};
    }
    return std::string(buffer, buffer + size);
}

llama_sampler* LocalLlmService::CreateSamplerChain(llama_model* model, const LocalLlmConfig& config, std::string& diagnostics, bool& usedGrammar) const
{
    usedGrammar = false;
    llama_sampler_chain_params samplerParams = llama_sampler_chain_default_params();
    llama_sampler* sampler = llama_sampler_chain_init(samplerParams);
    if (!sampler)
    {
        diagnostics = "llama_sampler_chain_init failed.";
        return nullptr;
    }

    if (config.structuredJson)
    {
        const llama_vocab* vocab = llama_model_get_vocab(model);
        llama_sampler* grammar = llama_sampler_init_grammar(vocab, LookDevJsonGrammar(), "root");
        if (grammar)
        {
            llama_sampler_chain_add(sampler, grammar);
            usedGrammar = true;
            diagnostics = "Structured JSON grammar enabled.";
        }
        else
        {
            diagnostics = "Structured JSON grammar failed to initialize; falling back to unconstrained generation.";
        }
    }

    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(std::max(1, config.topK)));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(std::clamp(config.topP, 0.05f, 1.0f), 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(std::clamp(config.temperature, 0.0f, 2.0f)));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    return sampler;
}

LocalLlmService::GenerationResult LocalLlmService::Generate(const std::vector<LocalLlmMessage>& messages)
{
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    llama_sampler* sampler = nullptr;
    LocalLlmConfig config;
    bool grammarActive = false;
    std::string samplerDiagnostics;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        model = m_model;
        context = m_context;
        sampler = m_sampler;
        config = m_config;
        grammarActive = m_structuredGrammarActive;
        samplerDiagnostics = m_samplerDiagnostics;
    }
    if (!model || !context || !sampler)
    {
        throw std::runtime_error("Model is not loaded.");
    }

    const auto generationStart = std::chrono::steady_clock::now();
    GenerationResult result;
    llama_memory_clear(llama_get_memory(context), true);
    llama_sampler_reset(sampler);
    const std::string prompt = ApplyChatTemplate(messages);
    std::vector<int> promptTokens = Tokenize(prompt, true, true);
    result.promptTokens = static_cast<int>(promptTokens.size());
    result.usedGrammar = grammarActive;
    result.diagnostics = samplerDiagnostics;
    if (promptTokens.empty())
    {
        throw std::runtime_error("Prompt tokenization produced no tokens.");
    }
    const std::size_t requiredTokens = promptTokens.size() + static_cast<std::size_t>(std::max(1, config.maxTokens));
    if (requiredTokens >= static_cast<std::size_t>(llama_n_ctx(context)))
    {
        throw std::runtime_error("Prompt does not fit in the configured context.");
    }

    llama_batch batch = llama_batch_init(static_cast<int32_t>(std::max<std::size_t>(promptTokens.size(), 1)), 0, 1);
    FillBatch(batch, promptTokens, 0, true);
    if (llama_decode(context, batch) != 0)
    {
        llama_batch_free(batch);
        throw std::runtime_error("llama_decode failed while processing the prompt.");
    }
    llama_batch_free(batch);

    int position = static_cast<int>(promptTokens.size());
    result.finishReason = "max_tokens";
    for (int i = 0; i < std::max(1, config.maxTokens) && !m_stopRequested; ++i)
    {
        const int token = llama_sampler_sample(sampler, context, -1);
        llama_sampler_accept(sampler, token);
        const llama_vocab* vocab = llama_model_get_vocab(model);
        if (llama_vocab_is_eog(vocab, token))
        {
            result.finishReason = "eog";
            break;
        }
        result.text += TokenToPiece(token);
        ++result.outputTokens;
        std::vector<int> nextToken = { token };
        llama_batch nextBatch = llama_batch_init(1, 0, 1);
        FillBatch(nextBatch, nextToken, position, true);
        ++position;
        const int decodeResult = llama_decode(context, nextBatch);
        llama_batch_free(nextBatch);
        if (decodeResult != 0)
        {
            result.finishReason = "decode_failed";
            break;
        }
    }
    if (m_stopRequested)
    {
        result.finishReason = "stopped";
    }
    const auto generationEnd = std::chrono::steady_clock::now();
    result.elapsedMs = std::chrono::duration<double, std::milli>(generationEnd - generationStart).count();
    return result;
}

std::vector<LocalLlmEvent> LocalLlmService::DrainEvents()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<LocalLlmEvent> events(m_events.begin(), m_events.end());
    m_events.clear();
    return events;
}

LocalLlmStatus LocalLlmService::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    LocalLlmStatus status;
    status.state = m_state;
    status.stateText = m_stateText;
    status.lastError = m_lastError;
    status.modelPath = m_config.modelPath;
    status.contextTokens = m_config.contextTokens;
    status.gpuLayers = m_config.gpuLayers;
    return status;
}

void LocalLlmService::PushEvent(LocalLlmEvent::Kind kind, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back({ kind, text });
}

void LocalLlmService::PushResponseEvent(const GenerationResult& result)
{
    LocalLlmEvent event;
    event.kind = LocalLlmEvent::Kind::Response;
    event.text = result.text;
    event.rawText = result.text;
    event.promptTokens = result.promptTokens;
    event.outputTokens = result.outputTokens;
    event.elapsedMs = result.elapsedMs;
    event.finishReason = result.finishReason;
    event.usedGrammar = result.usedGrammar;
    event.diagnostics = result.diagnostics;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(std::move(event));
}

void LocalLlmService::SetState(LocalLlmState state, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
    m_stateText = text.empty() ? StateName(state) : text;
}

void LocalLlmService::SetError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastError = error;
}
}
