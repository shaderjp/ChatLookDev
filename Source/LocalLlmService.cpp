#include "LocalLlmService.h"

#include <ggml-backend.h>
#include <llama.h>
#include <speculative.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
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

std::string FirstGpuDeviceDescription()
{
    for (std::size_t i = 0; i < ggml_backend_dev_count(); ++i)
    {
        ggml_backend_dev_t device = ggml_backend_dev_get(i);
        if (!device)
        {
            continue;
        }
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(device);
        if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU)
        {
            const char* name = ggml_backend_dev_name(device);
            const char* description = ggml_backend_dev_description(device);
            std::string text = name && name[0] ? name : "GPU";
            if (description && description[0])
            {
                text += " - ";
                text += description;
            }
            return text;
        }
    }
    return {};
}

std::string InferenceModeText(const cld::LocalLlmConfig& config, bool gpuAvailable, const std::string& gpuDevice)
{
    if (config.gpuLayers > 0 && gpuAvailable)
    {
        std::ostringstream text;
        text << "GPU offload (" << config.gpuLayers << " layer";
        if (config.gpuLayers != 1)
        {
            text << "s";
        }
        if (!gpuDevice.empty())
        {
            text << ", " << gpuDevice;
        }
        text << ")";
        return text.str();
    }
    if (config.gpuLayers > 0)
    {
        return "CPU (GPU offload unavailable; requested layers > 0)";
    }
    return "CPU";
}

int ClampMtpDraftTokens(int value)
{
    return std::clamp(value, 1, 16);
}

void AppendDiagnostics(std::string& diagnostics, const std::string& text)
{
    if (text.empty())
    {
        return;
    }
    if (!diagnostics.empty())
    {
        diagnostics += "\n";
    }
    diagnostics += text;
}

std::string MtpModeText(const cld::LocalLlmConfig& config, bool available)
{
    if (!config.mtpEnabled)
    {
        return "MTP disabled.";
    }

    std::ostringstream text;
    text << "MTP " << (available ? "enabled" : "unavailable")
         << " (draft max " << ClampMtpDraftTokens(config.mtpDraftTokens);
    if (!config.mtpDraftModelPath.empty())
    {
        text << ", separate draft GGUF";
    }
    else
    {
        text << ", target MTP context";
    }
    text << ").";
    return text.str();
}

common_params_speculative BuildMtpSpeculativeParams(const cld::LocalLlmConfig& config, llama_context* targetContext, llama_context* draftContext)
{
    common_params_speculative params;
    params.types = { COMMON_SPECULATIVE_TYPE_DRAFT_MTP };
    params.draft.ctx_tgt = targetContext;
    params.draft.ctx_dft = draftContext;
    params.draft.n_max = ClampMtpDraftTokens(config.mtpDraftTokens);
    params.draft.n_min = 0;
    params.draft.p_min = std::clamp(config.mtpMinP, 0.0f, 1.0f);
    params.draft.backend_sampling = config.mtpBackendSampling;
    return params;
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

bool DecodeTokensInChunks(
    llama_context* context,
    const std::vector<int>& tokens,
    int startPos,
    int32_t maxBatchTokens,
    std::string& diagnostics,
    common_speculative* speculative = nullptr)
{
    if (tokens.empty())
    {
        return true;
    }

    const int32_t batchCapacity = std::max<int32_t>(1, maxBatchTokens);
    llama_batch batch = llama_batch_init(batchCapacity, 0, 1);
    for (std::size_t offset = 0; offset < tokens.size(); offset += static_cast<std::size_t>(batchCapacity))
    {
        const int32_t chunkTokens = static_cast<int32_t>(std::min<std::size_t>(static_cast<std::size_t>(batchCapacity), tokens.size() - offset));
        batch.n_tokens = chunkTokens;
        for (int32_t i = 0; i < chunkTokens; ++i)
        {
            const std::size_t tokenIndex = offset + static_cast<std::size_t>(i);
            batch.token[i] = tokens[tokenIndex];
            batch.pos[i] = startPos + static_cast<int>(tokenIndex);
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = tokenIndex + 1 == tokens.size();
        }

        const int decodeResult = llama_decode(context, batch);
        if (decodeResult != 0)
        {
            llama_batch_free(batch);
            std::ostringstream message;
            message << "llama_decode failed while processing prompt chunk at token " << offset
                    << " (" << chunkTokens << " tokens, ret=" << decodeResult << ").";
            diagnostics = message.str();
            return false;
        }
        if (speculative && !common_speculative_process(speculative, batch))
        {
            llama_batch_free(batch);
            std::ostringstream message;
            message << "common_speculative_process failed while processing prompt chunk at token " << offset << ".";
            diagnostics = message.str();
            return false;
        }
    }

    llama_batch_free(batch);
    return true;
}

bool TrimSequenceAfter(llama_context* context, llama_pos keepEnd, std::string& diagnostics, const char* label)
{
    if (!context)
    {
        return true;
    }
    if (!llama_memory_seq_rm(llama_get_memory(context), 0, keepEnd, -1))
    {
        std::ostringstream message;
        message << "Failed to remove speculative " << label << " tokens after position " << keepEnd << ".";
        diagnostics = message.str();
        return false;
    }
    return true;
}

struct DraftVerification
{
    std::vector<int> emittedTokens;
    int acceptedDraftTokens = 0;
    bool hitEog = false;
};

DraftVerification SampleAndAcceptDraft(llama_sampler* sampler, llama_context* context, const llama_vocab* vocab, const std::vector<int>& draft)
{
    DraftVerification verification;
    verification.emittedTokens.reserve(draft.size() + 1);

    std::size_t i = 0;
    for (; i < draft.size(); ++i)
    {
        const int token = llama_sampler_sample(sampler, context, static_cast<int32_t>(i));
        if (llama_vocab_is_eog(vocab, token))
        {
            verification.acceptedDraftTokens = token == draft[i] ? static_cast<int>(i + 1) : static_cast<int>(i);
            verification.hitEog = true;
            return verification;
        }
        verification.emittedTokens.push_back(token);
        if (token != draft[i])
        {
            verification.acceptedDraftTokens = static_cast<int>(i);
            return verification;
        }
        verification.acceptedDraftTokens = static_cast<int>(i + 1);
    }

    const int token = llama_sampler_sample(sampler, context, static_cast<int32_t>(i));
    if (llama_vocab_is_eog(vocab, token))
    {
        verification.hitEog = true;
        return verification;
    }
    verification.emittedTokens.push_back(token);
    return verification;
}

const char* LookDevJsonGrammar()
{
    // Grammar narrows generation to known action names and JSON shape. Runtime
    // validation in ChatLookDevApp is still the authority for parameter safety.
    return R"gbnf(
root ::= ws "{" ws reply-field ws "," ws actions-field ws "}" ws
reply-field ::= "\"reply\"" ws ":" ws string
actions-field ::= "\"actions\"" ws ":" ws "[" ws (action (ws "," ws action)*)? ws "]"
action ::= "{" ws "\"method\"" ws ":" ws method ws "," ws "\"params\"" ws ":" ws object ws "}"
method ::= "\"set_view_settings\"" | "\"set_environment_settings\"" | "\"set_sun_settings\"" | "\"set_shadow_settings\"" | "\"set_material_preview\"" | "\"set_camera\"" | "\"set_model_transform\""
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
    if (config.mtpEnabled && !config.mtpDraftModelPath.empty() && !std::filesystem::exists(config.mtpDraftModelPath))
    {
        SetError("MTP draft GGUF model file was not found.");
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
    if (m_draftContext)
    {
        llama_free(m_draftContext);
        m_draftContext = nullptr;
    }
    if (m_context)
    {
        llama_free(m_context);
        m_context = nullptr;
    }
    if (m_draftModel)
    {
        llama_model_free(m_draftModel);
        m_draftModel = nullptr;
    }
    if (m_model)
    {
        llama_model_free(m_model);
        m_model = nullptr;
    }
    m_structuredGrammarActive = false;
    m_mtpAvailable = false;
    m_mtpMode.clear();
    m_samplerDiagnostics.clear();
}

void LocalLlmService::LoadWorker(LocalLlmConfig config)
{
    try
    {
        SetState(LocalLlmState::Loading, "Loading");
        PushEvent(LocalLlmEvent::Kind::Status, "Loading GGUF model...");

        const auto modelDeleter = [](llama_model* model)
        {
            if (model)
            {
                llama_model_free(model);
            }
        };
        const auto contextDeleter = [](llama_context* context)
        {
            if (context)
            {
                llama_free(context);
            }
        };
        using ModelPtr = std::unique_ptr<llama_model, decltype(modelDeleter)>;
        using ContextPtr = std::unique_ptr<llama_context, decltype(contextDeleter)>;

        llama_model_params modelParams = llama_model_default_params();
        modelParams.n_gpu_layers = std::max(0, config.gpuLayers);
        const std::string modelPath = PathToUtf8(config.modelPath);
        ModelPtr model(llama_model_load_from_file(modelPath.c_str(), modelParams), modelDeleter);
        if (!model)
        {
            throw std::runtime_error("llama_model_load_from_file failed.");
        }

        llama_context_params contextParams = llama_context_default_params();
        contextParams.n_ctx = std::max(1024, config.contextTokens);
        contextParams.n_batch = std::min<int32_t>(2048, contextParams.n_ctx);
        contextParams.n_ubatch = std::min<int32_t>(512, contextParams.n_batch);
        if (config.mtpEnabled)
        {
            contextParams.n_rs_seq = static_cast<uint32_t>(ClampMtpDraftTokens(config.mtpDraftTokens));
            contextParams.n_outputs_max = static_cast<uint32_t>(1 + ClampMtpDraftTokens(config.mtpDraftTokens));
        }

        ContextPtr context(llama_init_from_model(model.get(), contextParams), contextDeleter);
        if (!context)
        {
            throw std::runtime_error("llama_init_from_model failed.");
        }
        if (config.threads > 0)
        {
            llama_set_n_threads(context.get(), config.threads, config.threads);
        }

        ModelPtr draftModel(nullptr, modelDeleter);
        ContextPtr draftContext(nullptr, contextDeleter);
        bool mtpAvailable = false;
        std::string mtpDiagnostics = MtpModeText(config, false);

        if (config.mtpEnabled)
        {
            try
            {
                llama_model* draftModelForContext = model.get();
                if (!config.mtpDraftModelPath.empty())
                {
                    PushEvent(LocalLlmEvent::Kind::Status, "Loading MTP draft GGUF model...");
                    const std::string draftPath = PathToUtf8(config.mtpDraftModelPath);
                    draftModel.reset(llama_model_load_from_file(draftPath.c_str(), modelParams));
                    if (!draftModel)
                    {
                        throw std::runtime_error("llama_model_load_from_file failed for the MTP draft model.");
                    }
                    draftModelForContext = draftModel.get();
                }

                llama_context_params draftContextParams = contextParams;
                draftContextParams.ctx_type = LLAMA_CONTEXT_TYPE_MTP;
                draftContextParams.ctx_other = context.get();
                draftContextParams.n_rs_seq = 0;
                draftContextParams.n_outputs_max = static_cast<uint32_t>(1 + ClampMtpDraftTokens(config.mtpDraftTokens));
                draftContext.reset(llama_init_from_model(draftModelForContext, draftContextParams));
                if (!draftContext)
                {
                    throw std::runtime_error("llama_init_from_model failed for the MTP draft context.");
                }
                if (config.threads > 0)
                {
                    llama_set_n_threads(draftContext.get(), config.threads, config.threads);
                }

                common_params_speculative speculativeParams = BuildMtpSpeculativeParams(config, context.get(), draftContext.get());
                std::unique_ptr<common_speculative, common_speculative_deleter> speculativeProbe(common_speculative_init(speculativeParams, 1));
                if (!speculativeProbe)
                {
                    throw std::runtime_error("common_speculative_init failed for MTP.");
                }
                mtpAvailable = true;
                mtpDiagnostics = MtpModeText(config, true);
            }
            catch (const std::exception& ex)
            {
                draftContext.reset();
                draftModel.reset();
                if (!config.mtpDraftModelPath.empty())
                {
                    throw;
                }
                mtpAvailable = false;
                mtpDiagnostics = std::string("MTP requested but unavailable for this target GGUF; falling back to normal generation. Reason: ") + ex.what();
            }
        }

        std::string samplerDiagnostics;
        bool usedGrammar = false;
        llama_sampler* samplerProbe = CreateSamplerChain(model.get(), config, samplerDiagnostics, usedGrammar);
        if (!samplerProbe)
        {
            throw std::runtime_error("llama sampler initialization failed.");
        }
        llama_sampler_free(samplerProbe);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ReleaseModel();
            m_model = model.release();
            m_context = context.release();
            m_draftModel = draftModel.release();
            m_draftContext = draftContext.release();
            m_config = config;
            m_structuredGrammarActive = usedGrammar;
            m_mtpAvailable = mtpAvailable;
            m_mtpMode = mtpDiagnostics;
            m_samplerDiagnostics = samplerDiagnostics;
            m_lastError.clear();
        }
        SetState(LocalLlmState::Ready, "Ready");
        PushEvent(LocalLlmEvent::Kind::Status, "Local model ready.");
        PushEvent(LocalLlmEvent::Kind::Status, "Inference mode: " + InferenceModeText(config, llama_supports_gpu_offload(), FirstGpuDeviceDescription()));
        if (config.mtpEnabled)
        {
            PushEvent(LocalLlmEvent::Kind::Status, mtpDiagnostics);
        }
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
    llama_context* draftContext = nullptr;
    LocalLlmConfig config;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        model = m_model;
        context = m_context;
        draftContext = m_draftContext;
        config = m_config;
    }
    if (!model || !context)
    {
        throw std::runtime_error("Model is not loaded.");
    }
    if (config.mtpEnabled && draftContext)
    {
        return GenerateWithMtp(messages);
    }

    const auto generationStart = std::chrono::steady_clock::now();
    GenerationResult result;
    std::string samplerDiagnostics;
    bool grammarActive = false;
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        CreateSamplerChain(model, config, samplerDiagnostics, grammarActive),
        &llama_sampler_free);
    if (!sampler)
    {
        throw std::runtime_error("llama sampler initialization failed.");
    }

    llama_memory_clear(llama_get_memory(context), true);
    const std::string prompt = ApplyChatTemplate(messages);
    std::vector<int> promptTokens = Tokenize(prompt, true, true);
    result.promptTokens = static_cast<int>(promptTokens.size());
    result.usedGrammar = grammarActive;
    result.diagnostics = samplerDiagnostics;
    if (promptTokens.empty())
    {
        throw std::runtime_error("Prompt tokenization produced no tokens.");
    }
    const std::size_t contextTokens = static_cast<std::size_t>(llama_n_ctx(context));
    const std::size_t requiredTokens = promptTokens.size() + static_cast<std::size_t>(std::max(1, config.maxTokens));
    if (requiredTokens >= contextTokens)
    {
        std::ostringstream message;
        message << "Prompt does not fit in the configured context. Prompt tokens: "
                << promptTokens.size() << ", max reply tokens: " << std::max(1, config.maxTokens)
                << ", context tokens: " << contextTokens << ".";
        throw std::runtime_error(message.str());
    }

    const int32_t promptBatchTokens = std::max<int32_t>(1, static_cast<int32_t>(llama_n_batch(context)));
    std::string decodeDiagnostics;
    if (!DecodeTokensInChunks(context, promptTokens, 0, promptBatchTokens, decodeDiagnostics))
    {
        throw std::runtime_error(decodeDiagnostics);
    }
    if (promptTokens.size() > static_cast<std::size_t>(promptBatchTokens))
    {
        std::ostringstream chunkInfo;
        chunkInfo << "Prompt decoded in " << ((promptTokens.size() + static_cast<std::size_t>(promptBatchTokens) - 1) / static_cast<std::size_t>(promptBatchTokens))
                  << " chunks (batch limit " << promptBatchTokens << ").";
        if (!result.diagnostics.empty())
        {
            result.diagnostics += "\n";
        }
        result.diagnostics += chunkInfo.str();
    }

    int position = static_cast<int>(promptTokens.size());
    result.finishReason = "max_tokens";
    const llama_vocab* vocab = llama_model_get_vocab(model);
    for (int i = 0; i < std::max(1, config.maxTokens) && !m_stopRequested; ++i)
    {
        const int token = llama_sampler_sample(sampler.get(), context, -1);
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

LocalLlmService::GenerationResult LocalLlmService::GenerateWithMtp(const std::vector<LocalLlmMessage>& messages)
{
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    llama_context* draftContext = nullptr;
    LocalLlmConfig config;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        model = m_model;
        context = m_context;
        draftContext = m_draftContext;
        config = m_config;
    }
    if (!model || !context || !draftContext)
    {
        throw std::runtime_error("MTP context is not loaded.");
    }

    const auto generationStart = std::chrono::steady_clock::now();
    GenerationResult result;
    result.usedMtp = true;

    std::string samplerDiagnostics;
    bool grammarActive = false;
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
        CreateSamplerChain(model, config, samplerDiagnostics, grammarActive),
        &llama_sampler_free);
    if (!sampler)
    {
        throw std::runtime_error("llama sampler initialization failed.");
    }

    common_params_speculative speculativeParams = BuildMtpSpeculativeParams(config, context, draftContext);
    std::unique_ptr<common_speculative, common_speculative_deleter> speculative(common_speculative_init(speculativeParams, 1));
    if (!speculative)
    {
        throw std::runtime_error("common_speculative_init failed.");
    }

    llama_memory_clear(llama_get_memory(context), true);
    llama_memory_clear(llama_get_memory(draftContext), true);

    const std::string prompt = ApplyChatTemplate(messages);
    std::vector<int> promptTokens = Tokenize(prompt, true, true);
    result.promptTokens = static_cast<int>(promptTokens.size());
    result.usedGrammar = grammarActive;
    result.diagnostics = samplerDiagnostics;
    AppendDiagnostics(result.diagnostics, MtpModeText(config, true));
    if (promptTokens.empty())
    {
        throw std::runtime_error("Prompt tokenization produced no tokens.");
    }

    const int maxTokens = std::max(1, config.maxTokens);
    const std::size_t contextTokens = static_cast<std::size_t>(llama_n_ctx(context));
    const std::size_t requiredTokens = promptTokens.size() + static_cast<std::size_t>(maxTokens);
    if (requiredTokens >= contextTokens)
    {
        std::ostringstream message;
        message << "Prompt does not fit in the configured context. Prompt tokens: "
                << promptTokens.size() << ", max reply tokens: " << maxTokens
                << ", context tokens: " << contextTokens << ".";
        throw std::runtime_error(message.str());
    }

    const int32_t promptBatchTokens = std::max<int32_t>(1, static_cast<int32_t>(llama_n_batch(context)));
    std::string decodeDiagnostics;
    if (!DecodeTokensInChunks(context, promptTokens, 0, promptBatchTokens, decodeDiagnostics, speculative.get()))
    {
        throw std::runtime_error(decodeDiagnostics);
    }
    if (promptTokens.size() > static_cast<std::size_t>(promptBatchTokens))
    {
        std::ostringstream chunkInfo;
        chunkInfo << "Prompt decoded in " << ((promptTokens.size() + static_cast<std::size_t>(promptBatchTokens) - 1) / static_cast<std::size_t>(promptBatchTokens))
                  << " chunks (batch limit " << promptBatchTokens << ").";
        AppendDiagnostics(result.diagnostics, chunkInfo.str());
    }

    llama_tokens acceptedContextTokens(promptTokens.begin(), promptTokens.end());
    common_speculative_begin(speculative.get(), 0, acceptedContextTokens);

    const llama_vocab* vocab = llama_model_get_vocab(model);
    int sampled = llama_sampler_sample(sampler.get(), context, -1);
    if (llama_vocab_is_eog(vocab, sampled))
    {
        result.finishReason = "eog";
        result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - generationStart).count();
        return result;
    }

    result.text += TokenToPiece(sampled);
    ++result.outputTokens;

    llama_pos position = static_cast<llama_pos>(promptTokens.size());
    result.finishReason = "max_tokens";

    while (result.outputTokens < maxTokens && !m_stopRequested)
    {
        const int remainingTokens = maxTokens - result.outputTokens;
        const int draftLimit = std::min(ClampMtpDraftTokens(config.mtpDraftTokens), std::max(0, remainingTokens - 1));
        std::vector<int> draft;
        if (draftLimit > 0)
        {
            common_speculative_draft_params& draftParams = common_speculative_get_draft_params(speculative.get(), 0);
            draftParams.drafting = true;
            draftParams.n_max = draftLimit;
            draftParams.n_past = position;
            draftParams.id_last = sampled;
            draftParams.prompt = &acceptedContextTokens;
            draftParams.result = &draft;
            common_speculative_draft(speculative.get());
            if (static_cast<int>(draft.size()) > draftLimit)
            {
                draft.resize(static_cast<std::size_t>(draftLimit));
            }
            result.draftTokens += static_cast<int>(draft.size());
        }

        const int32_t batchTokens = static_cast<int32_t>(1 + draft.size());
        llama_batch batch = llama_batch_init(batchTokens, 0, 1);
        batch.n_tokens = batchTokens;
        batch.token[0] = sampled;
        batch.pos[0] = position;
        batch.n_seq_id[0] = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0] = true;
        for (int32_t i = 1; i < batchTokens; ++i)
        {
            batch.token[i] = draft[static_cast<std::size_t>(i - 1)];
            batch.pos[i] = position + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = true;
        }

        const int decodeResult = llama_decode(context, batch);
        if (decodeResult != 0)
        {
            llama_batch_free(batch);
            result.finishReason = "decode_failed";
            break;
        }
        if (!common_speculative_process(speculative.get(), batch))
        {
            llama_batch_free(batch);
            result.finishReason = "mtp_process_failed";
            AppendDiagnostics(result.diagnostics, "common_speculative_process failed during MTP verification.");
            break;
        }

        const DraftVerification verification = SampleAndAcceptDraft(sampler.get(), context, vocab, draft);
        llama_batch_free(batch);

        if (!draft.empty())
        {
            common_speculative_accept(speculative.get(), 0, static_cast<uint16_t>(verification.acceptedDraftTokens));
            result.acceptedDraftTokens += verification.acceptedDraftTokens;
        }

        acceptedContextTokens.push_back(sampled);
        for (int i = 0; i < verification.acceptedDraftTokens && i < static_cast<int>(draft.size()); ++i)
        {
            acceptedContextTokens.push_back(draft[static_cast<std::size_t>(i)]);
        }

        position += 1 + verification.acceptedDraftTokens;
        std::string trimDiagnostics;
        if (!TrimSequenceAfter(context, position, trimDiagnostics, "target"))
        {
            AppendDiagnostics(result.diagnostics, trimDiagnostics);
            result.finishReason = "mtp_trim_failed";
            break;
        }
        if (!TrimSequenceAfter(draftContext, position, trimDiagnostics, "draft"))
        {
            AppendDiagnostics(result.diagnostics, trimDiagnostics);
            result.finishReason = "mtp_trim_failed";
            break;
        }

        for (int token : verification.emittedTokens)
        {
            result.text += TokenToPiece(token);
            ++result.outputTokens;
            if (result.outputTokens >= maxTokens)
            {
                break;
            }
        }

        if (verification.hitEog)
        {
            result.finishReason = "eog";
            break;
        }
        if (verification.emittedTokens.empty())
        {
            result.finishReason = "eog";
            break;
        }
        sampled = verification.emittedTokens.back();
    }

    if (m_stopRequested)
    {
        result.finishReason = "stopped";
    }
    if (result.draftTokens > 0)
    {
        std::ostringstream stats;
        stats << "MTP draft acceptance: " << result.acceptedDraftTokens << "/" << result.draftTokens
              << " (" << (100.0 * static_cast<double>(result.acceptedDraftTokens) / static_cast<double>(std::max(1, result.draftTokens))) << "%).";
        AppendDiagnostics(result.diagnostics, stats.str());
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
    status.gpuOffloadAvailable = llama_supports_gpu_offload();
    status.inferenceDevice = FirstGpuDeviceDescription();
    status.inferenceMode = InferenceModeText(m_config, status.gpuOffloadAvailable, status.inferenceDevice);
    status.mtpRequested = m_config.mtpEnabled;
    status.mtpAvailable = m_mtpAvailable;
    status.mtpMode = m_mtpMode.empty() ? MtpModeText(m_config, m_mtpAvailable) : m_mtpMode;
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
    event.usedMtp = result.usedMtp;
    event.draftTokens = result.draftTokens;
    event.acceptedDraftTokens = result.acceptedDraftTokens;
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
