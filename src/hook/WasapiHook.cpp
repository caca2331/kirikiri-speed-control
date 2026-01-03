#include "WasapiHook.h"
#include "DirectSoundHook.h"
#include "HookUtils.h"
#include "SharedSettingsManager.h"
#include "SharedStatusManager.h"
#include "../common/Logging.h"
#include "../common/AudioStreamProcessor.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <atomic>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace krkrspeed {

namespace {
std::atomic<bool> g_loggedCoCreate{false};
std::atomic<bool> g_loggedEnumPatch{false};
std::atomic<bool> g_loggedDevicePatch{false};
std::atomic<bool> g_loggedAudioClientPatch{false};
std::atomic<bool> g_loggedRenderClientPatch{false};
std::atomic<bool> g_loggedAudioInit{false};
std::atomic<bool> g_loggedTempo{false};
std::atomic<bool> g_loggedSpeedup{false};
std::atomic<bool> g_loggedDsFallback{false};
std::atomic<bool> g_bootstrapDone{false};
std::atomic<bool> g_loggedDefaultFormat{false};
std::atomic<bool> g_loggedSilenceGate{false};
std::atomic<bool> g_loggedFirstNonSilentDrop{false};
std::atomic<void *> g_bootstrapAudioClient{nullptr};

const GUID kClsidMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
using PFN_GetDefaultAudioEndpoint =
    HRESULT(STDMETHODCALLTYPE *)(IMMDeviceEnumerator *, EDataFlow, ERole, IMMDevice **);
using PFN_GetDevice = HRESULT(STDMETHODCALLTYPE *)(IMMDeviceEnumerator *, LPCWSTR, IMMDevice **);
using PFN_DeviceActivate = HRESULT(STDMETHODCALLTYPE *)(IMMDevice *, REFIID, DWORD, PROPVARIANT *, void **);
using PFN_AudioClientInitialize =
    HRESULT(STDMETHODCALLTYPE *)(IAudioClient *, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME,
                                 const WAVEFORMATEX *, LPCGUID);
using PFN_AudioClientStart = HRESULT(STDMETHODCALLTYPE *)(IAudioClient *);
using PFN_AudioClientGetCurrentPadding = HRESULT(STDMETHODCALLTYPE *)(IAudioClient *, UINT32 *);
using PFN_AudioClientSetEventHandle = HRESULT(STDMETHODCALLTYPE *)(IAudioClient *, HANDLE);
using PFN_AudioClientGetService = HRESULT(STDMETHODCALLTYPE *)(IAudioClient *, REFIID, void **);
using PFN_RenderClientGetBuffer = HRESULT(STDMETHODCALLTYPE *)(IAudioRenderClient *, UINT32, BYTE **);
using PFN_RenderClientReleaseBuffer = HRESULT(STDMETHODCALLTYPE *)(IAudioRenderClient *, UINT32, DWORD);

PFN_GetDefaultAudioEndpoint g_origGetDefaultAudioEndpoint = nullptr;
PFN_GetDevice g_origGetDevice = nullptr;
PFN_DeviceActivate g_origDeviceActivate = nullptr;
PFN_AudioClientInitialize g_origAudioClientInitialize = nullptr;
PFN_AudioClientStart g_origAudioClientStart = nullptr;
PFN_AudioClientGetCurrentPadding g_origAudioClientGetCurrentPadding = nullptr;
PFN_AudioClientSetEventHandle g_origAudioClientSetEventHandle = nullptr;
PFN_AudioClientGetService g_origAudioClientGetService = nullptr;
PFN_RenderClientGetBuffer g_origRenderGetBuffer = nullptr;
PFN_RenderClientReleaseBuffer g_origRenderReleaseBuffer = nullptr;

struct StreamContext {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t blockAlign = 0;
    std::uint32_t dspBlockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t formatTag = 0;
    bool isPcm16 = false;
    bool isPcm32 = false;
    bool isFloat32 = false;
    bool formatGuessed = false;
    std::unique_ptr<AudioStreamProcessor> stream;
    std::mutex mutex;
    double targetFrames = 0.0;
    std::uint64_t outFrames = 0;
    std::int64_t lastEffective = 0;
};

struct RenderState {
    std::shared_ptr<StreamContext> ctx;
    BYTE *lastBuffer = nullptr;
    UINT32 lastFrames = 0;
    bool seenNonSilent = false;
    bool droppedFirstNonSilent = false;
};

void finalizeContextFormat(StreamContext &ctx, bool pcm16, bool pcm32, bool float32);

struct DefaultFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t formatTag = 0;
    bool isPcm16 = false;
    bool isPcm32 = false;
    bool isFloat32 = false;
};

std::mutex g_ctxMutex;
std::unordered_map<IAudioClient *, std::shared_ptr<StreamContext>> g_audioClients;
std::unordered_map<IAudioRenderClient *, RenderState> g_renderClients;
std::mutex g_formatMutex;
DefaultFormat g_defaultFormat;
bool g_haveDefaultFormat = false;

void ensureStream(StreamContext &ctx) {
    if (!ctx.stream && ctx.sampleRate > 0 && ctx.channels > 0 && ctx.dspBlockAlign > 0) {
        DspConfig cfg{};
        ctx.stream = std::make_unique<AudioStreamProcessor>(ctx.sampleRate, ctx.channels, ctx.dspBlockAlign, cfg);
    }
}

void parseFormatFlags(const WAVEFORMATEX *format, bool &pcm16, bool &pcm32, bool &float32) {
    pcm16 = false;
    pcm32 = false;
    float32 = false;
    if (!format) return;
    pcm16 = (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16);
    pcm32 = (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32);
    float32 = (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32);
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && format->wBitsPerSample == 16) {
            pcm16 = true;
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && format->wBitsPerSample == 32) {
            pcm32 = true;
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && format->wBitsPerSample == 32) {
            float32 = true;
        }
    }
}

DefaultFormat parseDefaultFormat(const WAVEFORMATEX *format) {
    DefaultFormat fmt{};
    if (!format) return fmt;
    fmt.sampleRate = format->nSamplesPerSec;
    fmt.channels = format->nChannels;
    fmt.blockAlign = format->nBlockAlign ? format->nBlockAlign : (format->nChannels * format->wBitsPerSample / 8);
    fmt.bitsPerSample = format->wBitsPerSample;
    fmt.formatTag = format->wFormatTag;
    parseFormatFlags(format, fmt.isPcm16, fmt.isPcm32, fmt.isFloat32);
    return fmt;
}

bool applyFormatToContext(StreamContext &ctx, const WAVEFORMATEX *format) {
    if (!format) return false;
    ctx.sampleRate = format->nSamplesPerSec;
    ctx.channels = format->nChannels;
    ctx.bitsPerSample = format->wBitsPerSample;
    ctx.formatTag = format->wFormatTag;
    bool pcm16 = false;
    bool pcm32 = false;
    bool float32 = false;
    parseFormatFlags(format, pcm16, pcm32, float32);
    finalizeContextFormat(ctx, pcm16, pcm32, float32);
    return (pcm16 || pcm32 || float32);
}

bool tryResolveChannelCountFromRenderClient(IAudioRenderClient *renderClient, StreamContext &ctx) {
    if (!renderClient) {
        return false;
    }
    IAudioClient *audioClient = nullptr;
    HRESULT hr = renderClient->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&audioClient));
    if (FAILED(hr) || !audioClient) {
        return false;
    }
    IAudioStreamVolume *volume = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioStreamVolume), reinterpret_cast<void **>(&volume));
    if (FAILED(hr) || !volume) {
        audioClient->Release();
        return false;
    }
    UINT32 channels = 0;
    hr = volume->GetChannelCount(&channels);
    volume->Release();
    audioClient->Release();
    if (FAILED(hr) || channels == 0) {
        return false;
    }
    if (ctx.channels != channels) {
        ctx.channels = channels;
        if (ctx.isFloat32 || ctx.isPcm32) {
            ctx.blockAlign = ctx.channels * sizeof(std::uint32_t);
        } else {
            ctx.blockAlign = ctx.channels * sizeof(std::int16_t);
        }
        ctx.dspBlockAlign = ctx.channels * sizeof(std::int16_t);
        ctx.stream.reset();
        ctx.formatGuessed = true;
        KRKR_LOG_INFO("WASAPI resolved channel count via IAudioStreamVolume: ch=" + std::to_string(channels));
    }
    return true;
}

bool tryResolveFormatFromRenderClient(IAudioRenderClient *renderClient, StreamContext &ctx) {
    if (!renderClient || !ctx.formatGuessed) {
        return false;
    }
    IAudioClient *audioClient = nullptr;
    HRESULT hr = renderClient->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&audioClient));
    if (FAILED(hr) || !audioClient) {
        return false;
    }
    WAVEFORMATEX *mix = nullptr;
    hr = audioClient->GetMixFormat(&mix);
    if (SUCCEEDED(hr) && mix) {
        const bool ok = applyFormatToContext(ctx, mix);
        CoTaskMemFree(mix);
        mix = nullptr;
        audioClient->Release();
        if (ok) {
            KRKR_LOG_INFO("WASAPI resolved stream format via render client");
            return true;
        }
    }
    audioClient->Release();
    return false;
}

void maybeSetDefaultFormat(const WAVEFORMATEX *format) {
    DefaultFormat fmt = parseDefaultFormat(format);
    if (fmt.sampleRate == 0 || fmt.channels == 0 || (!fmt.isPcm16 && !fmt.isPcm32 && !fmt.isFloat32)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_formatMutex);
    if (g_haveDefaultFormat) return;
    g_defaultFormat = fmt;
    g_haveDefaultFormat = true;
    if (!g_loggedDefaultFormat.exchange(true)) {
        std::string msg = "WASAPI default mix format captured";
        if (format) {
            msg += " tag=" + std::to_string(format->wFormatTag) +
                   " ch=" + std::to_string(format->nChannels) +
                   " rate=" + std::to_string(format->nSamplesPerSec) +
                   " bits=" + std::to_string(format->wBitsPerSample);
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
                if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                    msg += " sub=PCM";
                } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                    msg += " sub=FLOAT";
                } else {
                    msg += " sub=" + std::to_string(ext->SubFormat.Data1);
                }
            }
            if (fmt.isPcm16) {
                msg += " fmt=pcm16";
            } else if (fmt.isPcm32) {
                msg += " fmt=pcm32";
            } else if (fmt.isFloat32) {
                msg += " fmt=float32";
            }
        }
        KRKR_LOG_INFO(msg);
    }
}

std::shared_ptr<StreamContext> createContextFromDefault() {
    DefaultFormat fmt;
    {
        std::lock_guard<std::mutex> lock(g_formatMutex);
        if (!g_haveDefaultFormat) {
            return nullptr;
        }
        fmt = g_defaultFormat;
    }
    auto ctx = std::make_shared<StreamContext>();
    ctx->sampleRate = fmt.sampleRate;
    ctx->channels = fmt.channels;
    ctx->blockAlign = fmt.blockAlign;
    ctx->bitsPerSample = fmt.bitsPerSample;
    ctx->formatTag = fmt.formatTag;
    ctx->isPcm16 = fmt.isPcm16;
    ctx->isPcm32 = fmt.isPcm32;
    ctx->isFloat32 = fmt.isFloat32;
    ctx->formatGuessed = true;
    if (ctx->channels > 0) {
        ctx->dspBlockAlign = ctx->channels * sizeof(std::int16_t);
    }
    if (ctx->isPcm16 || ctx->isPcm32 || ctx->isFloat32) {
        DspConfig cfg{};
        ctx->stream = std::make_unique<AudioStreamProcessor>(ctx->sampleRate, ctx->channels, ctx->dspBlockAlign, cfg);
    }
    return ctx;
}

void finalizeContextFormat(StreamContext &ctx, bool pcm16, bool pcm32, bool float32) {
    ctx.isPcm16 = pcm16;
    ctx.isPcm32 = pcm32;
    ctx.isFloat32 = float32;
    ctx.formatGuessed = false;
    if (ctx.channels > 0) {
        ctx.blockAlign = ctx.channels * (float32 ? sizeof(float) : (pcm32 ? sizeof(std::int32_t) : sizeof(std::int16_t)));
        ctx.dspBlockAlign = ctx.channels * sizeof(std::int16_t);
    }
    if (pcm16 || pcm32 || float32) {
        if (!ctx.stream) {
            DspConfig cfg{};
            ctx.stream = std::make_unique<AudioStreamProcessor>(ctx.sampleRate, ctx.channels,
                                                                 ctx.dspBlockAlign, cfg);
        }
    }
}

bool looksLikePcm16(const BYTE *buffer, std::size_t bytes) {
    if (!buffer || bytes < sizeof(std::int16_t) * 8) {
        return false;
    }
    const std::size_t count = bytes / sizeof(std::int16_t);
    const auto *samples = reinterpret_cast<const std::int16_t *>(buffer);
    std::size_t smooth = 0;
    std::int64_t sumAbs = 0;
    for (std::size_t i = 1; i < count; ++i) {
        const std::int32_t cur = samples[i];
        const std::int32_t prev = samples[i - 1];
        sumAbs += std::abs(cur);
        if (std::abs(cur - prev) < 2000) {
            smooth++;
        }
    }
    const double avgAbs = static_cast<double>(sumAbs) / static_cast<double>(count);
    const double smoothPct = (count > 1) ? (static_cast<double>(smooth) * 100.0 / static_cast<double>(count - 1)) : 0.0;
    return (avgAbs > 100.0 && smoothPct > 12.0);
}

bool isBufferSilent(const StreamContext &ctx, const BYTE *buffer, UINT32 frames) {
    if (!buffer || frames == 0 || ctx.channels == 0) {
        return true;
    }
    const std::size_t maxSamples = std::min<std::size_t>(static_cast<std::size_t>(frames) * ctx.channels, 256);
    if (maxSamples == 0) {
        return true;
    }
    if (ctx.isFloat32) {
        const float *samples = reinterpret_cast<const float *>(buffer);
        float maxAbs = 0.0f;
        for (std::size_t i = 0; i < maxSamples; ++i) {
            const float v = samples[i];
            if (!std::isfinite(v)) {
                return false;
            }
            const float av = std::fabs(v);
            if (av > maxAbs) {
                maxAbs = av;
                if (maxAbs >= 1e-4f) {
                    return false;
                }
            }
        }
        return true;
    }
    if (ctx.isPcm32) {
        const std::int32_t *samples = reinterpret_cast<const std::int32_t *>(buffer);
        std::int32_t maxAbs = 0;
        for (std::size_t i = 0; i < maxSamples; ++i) {
            const std::int32_t v = samples[i];
            const std::int32_t av = (v == std::numeric_limits<std::int32_t>::min())
                ? std::numeric_limits<std::int32_t>::max()
                : std::abs(v);
            if (av > maxAbs) {
                maxAbs = av;
                if (maxAbs >= (32 << 16)) {
                    return false;
                }
            }
        }
        return true;
    }
    const std::int16_t *samples = reinterpret_cast<const std::int16_t *>(buffer);
    std::int16_t maxAbs = 0;
    for (std::size_t i = 0; i < maxSamples; ++i) {
        const std::int16_t v = samples[i];
        const std::int16_t av = (v == std::numeric_limits<std::int16_t>::min())
            ? std::numeric_limits<std::int16_t>::max()
            : static_cast<std::int16_t>(std::abs(v));
        if (av > maxAbs) {
            maxAbs = av;
            if (maxAbs >= 8) {
                return false;
            }
        }
    }
    return true;
}

void maybeAdjustGuessedFormat(StreamContext &ctx, const BYTE *buffer, UINT32 frames) {
    if (!ctx.formatGuessed || !buffer || frames == 0) {
        return;
    }
    const std::size_t maxBytes = std::min<std::size_t>(512, static_cast<std::size_t>(frames) * ctx.channels * 4);
    if (maxBytes < 8) {
        return;
    }
    const std::size_t wordBytes = maxBytes & ~static_cast<std::size_t>(3);
    const std::size_t wordCount = wordBytes / sizeof(std::uint32_t);
    if (wordCount == 0) {
        return;
    }
    const std::uint32_t *words = reinterpret_cast<const std::uint32_t *>(buffer);
    std::size_t normalCount = 0;
    std::size_t denormCount = 0;
    std::size_t nanInfCount = 0;
    std::size_t finiteCount = 0;
    std::size_t within2Count = 0;
    double maxAbs = 0.0;
    double sumAbs = 0.0;
    for (std::size_t i = 0; i < wordCount; ++i) {
        const std::uint32_t u = words[i];
        const std::uint32_t exp = (u >> 23) & 0xFFu;
        if (exp == 0) {
            denormCount++;
        } else if (exp == 0xFFu) {
            nanInfCount++;
        } else {
            normalCount++;
        }
        const float v = *reinterpret_cast<const float *>(&u);
        if (std::isfinite(v)) {
            const double av = std::fabs(static_cast<double>(v));
            sumAbs += av;
            finiteCount++;
            if (av <= 2.0) {
                within2Count++;
            }
            if (av > maxAbs) {
                maxAbs = av;
            }
        }
    }
    const std::size_t total = normalCount + denormCount + nanInfCount;
    if (total == 0 || finiteCount == 0) {
        return;
    }
    const double avgAbs = sumAbs / static_cast<double>(finiteCount);
    const double within2Pct = static_cast<double>(within2Count) * 100.0 / static_cast<double>(finiteCount);

    // If buffer is essentially silent, defer decision until we see real audio.
    if (avgAbs < 1e-5 && maxAbs < 1e-4) {
        return;
    }

    // Prefer float if values look like normalized audio.
    if (within2Pct >= 90.0 && nanInfCount * 100 < total * 1 && maxAbs <= 2.5) {
        finalizeContextFormat(ctx, false, false, true);
        KRKR_LOG_INFO("WASAPI guessed stream format: float32");
        return;
    }
    // If float interpretation is clearly invalid, fall back to PCM formats.
    if (maxAbs > 8.0 || nanInfCount * 100 >= total * 5) {
        if (looksLikePcm16(buffer, wordBytes)) {
            finalizeContextFormat(ctx, true, false, false);
            KRKR_LOG_INFO("WASAPI guessed stream format: pcm16");
        } else {
            // Keep current guess if we cannot validate PCM16.
        }
        return;
    }
    // If unsure, keep current guessed format (likely float32 from mix).
}

std::shared_ptr<StreamContext> ensureRenderContextLocked(IAudioRenderClient *client) {
    auto &entry = g_renderClients[client];
    if (!entry.ctx) {
        entry.ctx = createContextFromDefault();
    }
    return entry.ctx;
}

void PollSharedSettingsThrottled() {
    static std::chrono::steady_clock::time_point lastPoll{};
    const auto now = std::chrono::steady_clock::now();
    if (lastPoll.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPoll).count() > 200) {
        lastPoll = now;
        SharedSettingsManager::instance().pollSharedSettings();
    }
}

void patchRenderClient(IAudioRenderClient *client);
void patchAudioClient(IAudioClient *client);
void patchDevice(IMMDevice *device);
void patchEnumerator(IMMDeviceEnumerator *enumerator);
void bootstrapWasapiVtables();

HRESULT STDMETHODCALLTYPE RenderClientGetBufferHook(IAudioRenderClient *client, UINT32 numFramesRequested,
                                                     BYTE **ppData) {
    const HRESULT hr = g_origRenderGetBuffer ? g_origRenderGetBuffer(client, numFramesRequested, ppData) : E_FAIL;
    if (SUCCEEDED(hr) && ppData) {
        std::shared_ptr<StreamContext> ctx;
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        auto &entry = g_renderClients[client];
        entry.lastBuffer = *ppData;
        entry.lastFrames = numFramesRequested;
        ctx = entry.ctx ? entry.ctx : ensureRenderContextLocked(client);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE RenderClientReleaseBufferHook(IAudioRenderClient *client, UINT32 numFramesWritten, DWORD flags) {
    if (!g_origRenderReleaseBuffer) return E_FAIL;
    RenderState state;
    {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        auto it = g_renderClients.find(client);
        if (it != g_renderClients.end()) {
            if (!it->second.ctx) {
                it->second.ctx = ensureRenderContextLocked(client);
            }
            state = it->second;
            it->second.lastBuffer = nullptr;
            it->second.lastFrames = 0;
        } else {
            RenderState entry{};
            entry.ctx = ensureRenderContextLocked(client);
            g_renderClients[client] = entry;
            state = entry;
        }
    }
    if (state.ctx) {
        tryResolveChannelCountFromRenderClient(client, *state.ctx);
        if (!tryResolveFormatFromRenderClient(client, *state.ctx)) {
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                maybeAdjustGuessedFormat(*state.ctx, state.lastBuffer, numFramesWritten);
            }
        }
    }
    PollSharedSettingsThrottled();
    if (DirectSoundHook::instance().isActive()) {
        if (state.ctx) {
            std::lock_guard<std::mutex> lock(state.ctx->mutex);
            state.ctx->targetFrames = 0.0;
            state.ctx->outFrames = 0;
            state.ctx->lastEffective = 0;
        }
        if (!g_loggedDsFallback.exchange(true)) {
            KRKR_LOG_INFO("WASAPI fallback disabled: DirectSound activity detected");
        }
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }
    const float speed = SharedSettingsManager::instance().getUserSpeed();
    const bool speedupActive = speed > 1.01f;
    UINT32 effectiveFrames = numFramesWritten;
    bool dropFrames = false;

    auto ctx = state.ctx;
    std::unique_lock<std::mutex> ctxLock;
    if (ctx) {
        ctxLock = std::unique_lock<std::mutex>(ctx->mutex);
    }

    if (!speedupActive) {
        if (ctx) {
            ctx->targetFrames = 0.0;
            ctx->outFrames = 0;
            ctx->lastEffective = 0;
        }
        if (ctxLock.owns_lock()) ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }

    if (ctx && numFramesWritten > 0) {
        const double add = static_cast<double>(numFramesWritten) / static_cast<double>(std::max(0.01f, speed));
        ctx->targetFrames += add;
        const std::int64_t desiredTotal = static_cast<std::int64_t>(std::llround(ctx->targetFrames));
        std::int64_t eff = desiredTotal - static_cast<std::int64_t>(ctx->outFrames);
        const std::int64_t minFrames = 1;
        const std::int64_t maxFrames = static_cast<std::int64_t>(numFramesWritten);

        if (ctx->sampleRate > 0) {
            const std::int64_t maxDrift =
                static_cast<std::int64_t>(std::llround(static_cast<double>(ctx->sampleRate) * 0.03));
            if (maxDrift > 0) {
                const double minEffD = ctx->targetFrames - static_cast<double>(maxDrift) -
                                       static_cast<double>(ctx->outFrames);
                const double maxEffD = ctx->targetFrames + static_cast<double>(maxDrift) -
                                       static_cast<double>(ctx->outFrames);
                const std::int64_t minEff = static_cast<std::int64_t>(std::floor(minEffD));
                const std::int64_t maxEff = static_cast<std::int64_t>(std::ceil(maxEffD));
                eff = std::max(eff, minEff);
                eff = std::min(eff, maxEff);
            }
        }

        eff = std::max(eff, minFrames);
        eff = std::min(eff, maxFrames);

        effectiveFrames = static_cast<UINT32>(eff);
        dropFrames = (effectiveFrames < numFramesWritten);
        ctx->lastEffective = eff;
    }

    if (dropFrames && !g_loggedSpeedup.exchange(true)) {
        KRKR_LOG_INFO("WASAPI speedup drop mode active; reducing ReleaseBuffer frames");
    }

    if (!ctx || numFramesWritten == 0) {
        if (ctxLock.owns_lock()) ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, numFramesWritten, flags);
    }

    if (!state.lastBuffer || state.lastFrames == 0) {
        ctx->outFrames += effectiveFrames;
        if (ctxLock.owns_lock()) ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags);
    }

    const bool flaggedSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const bool sampleSilent = flaggedSilent ? true : isBufferSilent(*ctx, state.lastBuffer, numFramesWritten);
    if (!state.seenNonSilent && sampleSilent) {
        if (!g_loggedSilenceGate.exchange(true)) {
            KRKR_LOG_INFO("WASAPI initial silence gate: zeroing buffer until first non-silent audio");
        }
        std::memset(state.lastBuffer, 0, static_cast<std::size_t>(numFramesWritten) * ctx->blockAlign);
        ctx->outFrames += effectiveFrames;
        if (ctxLock.owns_lock()) ctxLock.unlock();
        {
            std::lock_guard<std::mutex> lock(g_ctxMutex);
            auto it = g_renderClients.find(client);
            if (it != g_renderClients.end()) {
                it->second.seenNonSilent = false;
            }
        }
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags | AUDCLNT_BUFFERFLAGS_SILENT);
    }

    if (!state.seenNonSilent && !state.droppedFirstNonSilent && !sampleSilent) {
        const float durSec = (ctx->sampleRate > 0)
            ? static_cast<float>(numFramesWritten) / static_cast<float>(ctx->sampleRate)
            : 0.0f;
        if (durSec > 0.0f && durSec < 1.0f) {
            if (!g_loggedFirstNonSilentDrop.exchange(true)) {
                KRKR_LOG_INFO("WASAPI dropping first non-silent buffer to avoid startup click");
            }
            std::memset(state.lastBuffer, 0, static_cast<std::size_t>(numFramesWritten) * ctx->blockAlign);
            ctx->outFrames += effectiveFrames;
            if (ctxLock.owns_lock()) ctxLock.unlock();
            {
                std::lock_guard<std::mutex> lock(g_ctxMutex);
                auto it = g_renderClients.find(client);
                if (it != g_renderClients.end()) {
                    it->second.droppedFirstNonSilent = true;
                }
            }
            return g_origRenderReleaseBuffer(client, effectiveFrames, flags | AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    if (!sampleSilent) {
        SharedStatusManager::instance().setActiveBackend(AudioBackend::Wasapi);
    }

    if (flaggedSilent) {
        ctx->outFrames += effectiveFrames;
        if (ctxLock.owns_lock()) ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags);
    }

    if ((!ctx->isPcm16 && !ctx->isPcm32 && !ctx->isFloat32) || ctx->blockAlign == 0) {
        ctx->outFrames += effectiveFrames;
        if (ctxLock.owns_lock()) ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags);
    }

    ensureStream(*ctx);
    if (!ctx->stream) {
        ctx->outFrames += effectiveFrames;
        if (ctxLock.owns_lock()) ctxLock.unlock();
        return g_origRenderReleaseBuffer(client, effectiveFrames, flags);
    }

    const std::size_t inputBytes = static_cast<std::size_t>(numFramesWritten) * ctx->blockAlign;
    const std::size_t outputBytes = static_cast<std::size_t>(effectiveFrames) * ctx->blockAlign;
    const float durationSec = (ctx->sampleRate > 0)
        ? static_cast<float>(numFramesWritten) / static_cast<float>(ctx->sampleRate)
        : 0.0f;

    const auto now = std::chrono::steady_clock::now();
    ctx->stream->resetIfIdle(now, std::chrono::milliseconds(200), false,
                             reinterpret_cast<std::uintptr_t>(client));
    std::size_t cbufSize = 0;
    if (ctx->isFloat32) {
        const std::size_t inSamples = static_cast<std::size_t>(numFramesWritten) * ctx->channels;
        const std::size_t outSamples = static_cast<std::size_t>(effectiveFrames) * ctx->channels;
        std::vector<std::int16_t> tempIn(inSamples);
        const float *src = reinterpret_cast<const float *>(state.lastBuffer);
        for (std::size_t i = 0; i < inSamples; ++i) {
            float s = src ? src[i] : 0.0f;
            s = std::clamp(s, -1.0f, 1.0f);
            tempIn[i] = static_cast<std::int16_t>(std::lround(s * 32767.0f));
        }
        const std::size_t inputBytes16 = inSamples * sizeof(std::int16_t);
        const std::size_t outputBytes16 = outSamples * sizeof(std::int16_t);
        auto res = ctx->stream->processTempoToSize(reinterpret_cast<const std::uint8_t *>(tempIn.data()),
                                                   inputBytes16, outputBytes16, speed, false,
                                                   reinterpret_cast<std::uintptr_t>(client));
        cbufSize = res.cbufferSize;
        const std::size_t resSamples = res.output.size() / sizeof(std::int16_t);
        const auto *out16 = reinterpret_cast<const std::int16_t *>(res.output.data());
        float *dst = reinterpret_cast<float *>(state.lastBuffer);
        for (std::size_t i = 0; i < outSamples; ++i) {
            const std::int16_t v = (i < resSamples) ? out16[i] : 0;
            dst[i] = static_cast<float>(v) / 32768.0f;
        }
    } else if (ctx->isPcm32) {
        const std::size_t inSamples = static_cast<std::size_t>(numFramesWritten) * ctx->channels;
        const std::size_t outSamples = static_cast<std::size_t>(effectiveFrames) * ctx->channels;
        std::vector<std::int16_t> tempIn(inSamples);
        const std::int32_t *src = reinterpret_cast<const std::int32_t *>(state.lastBuffer);
        for (std::size_t i = 0; i < inSamples; ++i) {
            const std::int32_t s = src ? src[i] : 0;
            tempIn[i] = static_cast<std::int16_t>(s >> 16);
        }
        const std::size_t inputBytes16 = inSamples * sizeof(std::int16_t);
        const std::size_t outputBytes16 = outSamples * sizeof(std::int16_t);
        auto res = ctx->stream->processTempoToSize(reinterpret_cast<const std::uint8_t *>(tempIn.data()),
                                                   inputBytes16, outputBytes16, speed, false,
                                                   reinterpret_cast<std::uintptr_t>(client));
        cbufSize = res.cbufferSize;
        const std::size_t resSamples = res.output.size() / sizeof(std::int16_t);
        const auto *out16 = reinterpret_cast<const std::int16_t *>(res.output.data());
        std::int32_t *dst = reinterpret_cast<std::int32_t *>(state.lastBuffer);
        for (std::size_t i = 0; i < outSamples; ++i) {
            const std::int16_t v = (i < resSamples) ? out16[i] : 0;
            dst[i] = static_cast<std::int32_t>(v) << 16;
        }
    } else {
        auto res = ctx->stream->processTempoToSize(reinterpret_cast<const std::uint8_t *>(state.lastBuffer),
                                                   inputBytes, outputBytes, speed, false,
                                                   reinterpret_cast<std::uintptr_t>(client));
        cbufSize = res.cbufferSize;
        if (!res.output.empty()) {
            const std::size_t copyBytes = std::min(res.output.size(), inputBytes);
            std::memcpy(state.lastBuffer, res.output.data(), copyBytes);
        }
    }
    ctx->stream->recordPlaybackEnd(durationSec, speed);
    ctx->outFrames += effectiveFrames;

    if (!g_loggedTempo.exchange(true)) {
        KRKR_LOG_INFO("WASAPI tempo speedup active speed=" + std::to_string(speed) +
                      " cbuf=" + std::to_string(cbufSize));
    }

    if (ctxLock.owns_lock()) ctxLock.unlock();
    if (!state.seenNonSilent) {
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        auto it = g_renderClients.find(client);
        if (it != g_renderClients.end()) {
            it->second.seenNonSilent = true;
        }
    }
    return g_origRenderReleaseBuffer(client, effectiveFrames, flags & ~AUDCLNT_BUFFERFLAGS_SILENT);
}

HRESULT STDMETHODCALLTYPE AudioClientGetServiceHook(IAudioClient *client, REFIID riid, void **ppv) {
    const HRESULT hr = g_origAudioClientGetService ? g_origAudioClientGetService(client, riid, ppv) : E_FAIL;
    if (SUCCEEDED(hr) && ppv && *ppv && IsEqualIID(riid, __uuidof(IAudioRenderClient))) {
        if (!g_loggedRenderClientPatch.exchange(true)) {
            KRKR_LOG_INFO("WASAPI IAudioClient::GetService returned IAudioRenderClient");
        }
        patchRenderClient(reinterpret_cast<IAudioRenderClient *>(*ppv));
        std::lock_guard<std::mutex> lock(g_ctxMutex);
        auto it = g_audioClients.find(client);
        if (it != g_audioClients.end()) {
            g_renderClients[reinterpret_cast<IAudioRenderClient *>(*ppv)].ctx = it->second;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE AudioClientGetCurrentPaddingHook(IAudioClient *client, UINT32 *padding) {
    const HRESULT hr =
        g_origAudioClientGetCurrentPadding ? g_origAudioClientGetCurrentPadding(client, padding) : E_FAIL;
    return hr;
}

HRESULT STDMETHODCALLTYPE AudioClientSetEventHandleHook(IAudioClient *client, HANDLE hEvent) {
    return g_origAudioClientSetEventHandle ? g_origAudioClientSetEventHandle(client, hEvent) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE AudioClientInitializeHook(IAudioClient *client, AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                                                     REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
                                                     const WAVEFORMATEX *format, LPCGUID sessionGuid) {
    if (!g_loggedAudioInit.exchange(true)) {
        std::string msg = "WASAPI IAudioClient::Initialize";
        msg += (shareMode == AUDCLNT_SHAREMODE_SHARED) ? " shared" : " exclusive";
        if (format) {
            msg += " ch=" + std::to_string(format->nChannels) +
                   " rate=" + std::to_string(format->nSamplesPerSec) +
                   " bits=" + std::to_string(format->wBitsPerSample) +
                   " tag=" + std::to_string(format->wFormatTag);
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
                if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                    msg += " sub=PCM";
                } else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                    msg += " sub=FLOAT";
                } else {
                    msg += " sub=" + std::to_string(ext->SubFormat.Data1);
                }
            }
            if (client == g_bootstrapAudioClient.load()) {
                msg += " src=bootstrap";
            }
        }
        KRKR_LOG_INFO(msg);
    }
    if (format) {
        auto ctx = std::make_shared<StreamContext>();
        ctx->sampleRate = format->nSamplesPerSec;
        ctx->channels = format->nChannels;
        ctx->blockAlign = format->nBlockAlign ? format->nBlockAlign : (format->nChannels * format->wBitsPerSample / 8);
        ctx->bitsPerSample = format->wBitsPerSample;
        ctx->formatTag = format->wFormatTag;
        bool pcm16 = false;
        bool pcm32 = false;
        bool float32 = false;
        parseFormatFlags(format, pcm16, pcm32, float32);
        ctx->isPcm16 = pcm16;
        ctx->isPcm32 = pcm32;
        ctx->isFloat32 = float32;
        ctx->formatGuessed = false;
        maybeSetDefaultFormat(format);
        if (pcm16 || pcm32 || float32) {
            ctx->dspBlockAlign = ctx->channels * sizeof(std::int16_t);
            DspConfig cfg{};
            ctx->stream = std::make_unique<AudioStreamProcessor>(ctx->sampleRate, ctx->channels,
                                                                 ctx->dspBlockAlign, cfg);
        }
        {
            std::lock_guard<std::mutex> lock(g_ctxMutex);
            g_audioClients[client] = ctx;
        }
        if (!pcm16 && !pcm32 && !float32) {
            KRKR_LOG_WARN("WASAPI format not PCM16/PCM32/Float32; processing disabled for this stream");
        }
    }
    return g_origAudioClientInitialize ? g_origAudioClientInitialize(client, shareMode, streamFlags, hnsBufferDuration,
                                                                      hnsPeriodicity, format, sessionGuid)
                                       : E_FAIL;
}

HRESULT STDMETHODCALLTYPE AudioClientStartHook(IAudioClient *client) {
    return g_origAudioClientStart ? g_origAudioClientStart(client) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE DeviceActivateHook(IMMDevice *device, REFIID iid, DWORD clsctx, PROPVARIANT *params, void **ppv) {
    const HRESULT hr = g_origDeviceActivate ? g_origDeviceActivate(device, iid, clsctx, params, ppv) : E_FAIL;
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (IsEqualIID(iid, __uuidof(IAudioClient))) {
            patchAudioClient(reinterpret_cast<IAudioClient *>(*ppv));
        } else if (IsEqualIID(iid, __uuidof(IAudioRenderClient))) {
            if (!g_loggedRenderClientPatch.exchange(true)) {
                KRKR_LOG_INFO("WASAPI IMMDevice::Activate -> IAudioRenderClient");
            }
            patchRenderClient(reinterpret_cast<IAudioRenderClient *>(*ppv));
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpointHook(IMMDeviceEnumerator *enumerator, EDataFlow flow, ERole role,
                                                       IMMDevice **ppDevice) {
    const HRESULT hr = g_origGetDefaultAudioEndpoint ? g_origGetDefaultAudioEndpoint(enumerator, flow, role, ppDevice) : E_FAIL;
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        patchDevice(*ppDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDeviceHook(IMMDeviceEnumerator *enumerator, LPCWSTR id, IMMDevice **ppDevice) {
    const HRESULT hr = g_origGetDevice ? g_origGetDevice(enumerator, id, ppDevice) : E_FAIL;
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        patchDevice(*ppDevice);
    }
    return hr;
}

void patchEnumerator(IMMDeviceEnumerator *enumerator) {
    if (!enumerator) return;
    void **vtbl = *reinterpret_cast<void ***>(enumerator);
    bool patched = false;
    if (!g_origGetDefaultAudioEndpoint) {
        patched |= PatchVtableEntry(vtbl, 4, &GetDefaultAudioEndpointHook, g_origGetDefaultAudioEndpoint);
    }
    if (!g_origGetDevice) {
        patched |= PatchVtableEntry(vtbl, 5, &GetDeviceHook, g_origGetDevice);
    }
    if (patched && !g_loggedEnumPatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IMMDeviceEnumerator vtable patched");
    }
}

void patchDevice(IMMDevice *device) {
    if (!device) return;
    void **vtbl = *reinterpret_cast<void ***>(device);
    bool patched = false;
    if (!g_origDeviceActivate) {
        patched |= PatchVtableEntry(vtbl, 3, &DeviceActivateHook, g_origDeviceActivate);
    }
    if (patched && !g_loggedDevicePatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IMMDevice vtable patched");
    }
}

void patchAudioClient(IAudioClient *client) {
    if (!client) return;
    void **vtbl = *reinterpret_cast<void ***>(client);
    bool patched = false;
    if (!g_origAudioClientInitialize) {
        patched |= PatchVtableEntry(vtbl, 3, &AudioClientInitializeHook, g_origAudioClientInitialize);
    }
    if (!g_origAudioClientGetCurrentPadding) {
        patched |= PatchVtableEntry(vtbl, 6, &AudioClientGetCurrentPaddingHook, g_origAudioClientGetCurrentPadding);
    }
    if (!g_origAudioClientStart) {
        patched |= PatchVtableEntry(vtbl, 10, &AudioClientStartHook, g_origAudioClientStart);
    }
    if (!g_origAudioClientSetEventHandle) {
        patched |= PatchVtableEntry(vtbl, 13, &AudioClientSetEventHandleHook, g_origAudioClientSetEventHandle);
    }
    if (!g_origAudioClientGetService) {
        patched |= PatchVtableEntry(vtbl, 14, &AudioClientGetServiceHook, g_origAudioClientGetService);
    }
    if (patched && !g_loggedAudioClientPatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IAudioClient vtable patched");
    }
}

void patchRenderClient(IAudioRenderClient *client) {
    if (!client) return;
    void **vtbl = *reinterpret_cast<void ***>(client);
    bool patched = false;
    if (!g_origRenderGetBuffer) {
        patched |= PatchVtableEntry(vtbl, 3, &RenderClientGetBufferHook, g_origRenderGetBuffer);
    }
    if (!g_origRenderReleaseBuffer) {
        patched |= PatchVtableEntry(vtbl, 4, &RenderClientReleaseBufferHook, g_origRenderReleaseBuffer);
    }
    if (patched && !g_loggedRenderClientPatch.exchange(true)) {
        KRKR_LOG_INFO("WASAPI IAudioRenderClient vtable patched");
    }
}

void bootstrapWasapiVtables() {
    if (g_bootstrapDone.exchange(true)) {
        return;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        KRKR_LOG_WARN("WASAPI bootstrap: CoInitializeEx failed");
        return;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = ::CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_INPROC_SERVER,
                            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        KRKR_LOG_WARN("WASAPI bootstrap: CoCreateInstance(IMMDeviceEnumerator) failed");
        if (needUninit) {
            CoUninitialize();
        }
        return;
    }
    patchEnumerator(enumerator);

    IMMDevice *device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    }
    if (SUCCEEDED(hr) && device) {
        patchDevice(device);
    }

    IAudioClient *audioClient = nullptr;
    if (device) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                              reinterpret_cast<void **>(&audioClient));
    }
    if (SUCCEEDED(hr) && audioClient) {
        g_bootstrapAudioClient.store(audioClient);
        patchAudioClient(audioClient);
    }

    IAudioRenderClient *renderClient = nullptr;
    if (audioClient) {
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(audioClient->GetMixFormat(&mix)) && mix) {
            maybeSetDefaultFormat(mix);
            REFERENCE_TIME defaultPeriod = 0;
            if (FAILED(audioClient->GetDevicePeriod(&defaultPeriod, nullptr)) || defaultPeriod <= 0) {
                defaultPeriod = 1000000; // 100ms in 100ns units
            }
            const REFERENCE_TIME bufferDuration = defaultPeriod * 2;
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_NOPERSIST,
                                         bufferDuration,
                                         0,
                                         mix,
                                         nullptr);
            CoTaskMemFree(mix);
            mix = nullptr;
            if (SUCCEEDED(hr)) {
                hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                                             reinterpret_cast<void **>(&renderClient));
            }
        }
    }
    if (renderClient) {
        patchRenderClient(renderClient);
    } else {
        KRKR_LOG_WARN("WASAPI bootstrap: IAudioRenderClient not available");
    }

    if (renderClient) {
        renderClient->Release();
    }
    if (audioClient) {
        audioClient->Release();
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (needUninit) {
        CoUninitialize();
    }
    KRKR_LOG_INFO("WASAPI bootstrap vtable patch complete");
}

} // namespace

WasapiHook &WasapiHook::instance() {
    static WasapiHook hook;
    return hook;
}

void WasapiHook::initialize() {
    tryPatchModule(GetModuleHandleW(nullptr), "main");
    HMODULE unity = GetModuleHandleA("UnityPlayer.dll");
    if (unity) {
        tryPatchModule(unity, "UnityPlayer.dll");
    }
    bootstrapWasapiVtables();

    if (!m_origCoCreate) {
        m_origCoCreate = ::CoCreateInstance;
        if (!m_loggedInit.exchange(true)) {
            KRKR_LOG_DEBUG("WASAPI using CoCreateInstance fallback (no import patch)");
        }
    }
}

void WasapiHook::tryPatchModule(HMODULE module, const char *moduleName) {
    bool patched = false;
    if (!m_origCoCreate) {
        void *orig = nullptr;
        if (PatchImportInModule(module, "ole32.dll", "CoCreateInstance",
                                reinterpret_cast<void *>(&WasapiHook::CoCreateInstanceHook), &orig)) {
            m_origCoCreate = reinterpret_cast<PFN_CoCreateInstance>(orig);
            patched = true;
        }
    }
    if (!m_origCoCreate) {
        void *orig = nullptr;
        if (PatchImportInModule(module, "combase.dll", "CoCreateInstance",
                                reinterpret_cast<void *>(&WasapiHook::CoCreateInstanceHook), &orig)) {
            m_origCoCreate = reinterpret_cast<PFN_CoCreateInstance>(orig);
            patched = true;
        }
    }
    if (patched) {
        KRKR_LOG_INFO(std::string("WASAPI CoCreateInstance patched (") + moduleName + ")");
    }
}

void WasapiHook::setOriginalCoCreate(void *fn) {
    if (!fn || m_origCoCreate) {
        return;
    }
    m_origCoCreate = reinterpret_cast<PFN_CoCreateInstance>(fn);
}

HRESULT WINAPI WasapiHook::CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                                REFIID riid, LPVOID *ppv) {
    auto &self = WasapiHook::instance();
    auto fn = self.m_origCoCreate ? self.m_origCoCreate : ::CoCreateInstance;

    const HRESULT hr = fn(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (IsEqualCLSID(rclsid, kClsidMMDeviceEnumerator)) {
            if (!g_loggedCoCreate.exchange(true)) {
                KRKR_LOG_INFO("WASAPI CoCreateInstance -> IMMDeviceEnumerator");
            }
            patchEnumerator(reinterpret_cast<IMMDeviceEnumerator *>(*ppv));
        }
    }
    return hr;
}

} // namespace krkrspeed
