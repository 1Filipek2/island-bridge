#include "media/audio.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{



float SampleAudioAmplitude(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
    if (!data || !frames || !format || !format->nChannels) {
        return 0.0f;
    }

    double sum = 0.0;
    size_t samples = static_cast<size_t>(frames) * format->nChannels;

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                     kSubTypeIeeeFloat))) {
        auto* f = reinterpret_cast<float*>(data);
        for (size_t i = 0; i < samples; ++i) {
            sum += f[i] * f[i];
        }
    } else if (format->wBitsPerSample == 16) {
        auto* s = reinterpret_cast<int16_t*>(data);
        for (size_t i = 0; i < samples; ++i) {
            const double v = s[i] / 32768.0;
            sum += v * v;
        }
    }

    const double rms = samples ? std::sqrt(sum / samples) : 0.0;
    return Clamp(static_cast<float>(rms * 4.0), 0.0f, 1.0f);
}

// Goertzel filter bank over a sliding window of the mixed-down system audio.
// Produces kSpectrumBands log-spaced magnitudes (50 Hz .. 14 kHz) with an
// attack/release envelope and a slow auto-gain so quiet and loud tracks both
// fill the visualizer. Only ever touched from the audio thread.
struct SpectrumAnalyzer {
    static constexpr size_t kWindow = 1024;
    static constexpr size_t kHop = 512;

    static constexpr int kLog2Window = 10;  // kWindow == 1 << 10

    float ring[kWindow]{};
    float window[kWindow]{};
    float twCos[kWindow / 2]{};
    float twSin[kWindow / 2]{};
    uint16_t bitRev[kWindow]{};
    int   binLo[kSpectrumBands]{};  // FFT bins covered by each band, [lo, hi)
    int   binHi[kSpectrumBands]{};
    float smooth[kSpectrumBands]{};
    float agc = 0.04f;
    float sampleRate = 0.0f;
    size_t write = 0;
    size_t sinceHop = 0;
    bool windowReady = false;

    void Configure(float rate) {
        if (rate < 8000.0f) rate = 48000.0f;
        if (!windowReady) {
            for (size_t i = 0; i < kWindow; ++i) {
                window[i] = 0.5f - 0.5f * std::cos(6.2831853f * i / (kWindow - 1));
            }
            for (size_t i = 0; i < kWindow / 2; ++i) {
                const float a = -6.2831853f * static_cast<float>(i) / static_cast<float>(kWindow);
                twCos[i] = std::cos(a);
                twSin[i] = std::sin(a);
            }
            for (size_t i = 0; i < kWindow; ++i) {
                size_t r = 0;
                for (int bit = 0; bit < kLog2Window; ++bit) {
                    if (i & (static_cast<size_t>(1) << bit)) {
                        r |= static_cast<size_t>(1) << (kLog2Window - 1 - bit);
                    }
                }
                bitRev[i] = static_cast<uint16_t>(r);
            }
            windowReady = true;
        }
        if (rate == sampleRate) return;
        sampleRate = rate;
        const float fMin = 50.0f;
        const float fMax = std::min(14000.0f, rate * 0.45f);
        const float ratio = std::pow(fMax / fMin, 1.0f / (kSpectrumBands - 1));
        const float halfStep = std::sqrt(ratio);
        const float binHz = rate / kWindow;
        const int nyquistBin = static_cast<int>(kWindow / 2) - 1;
        for (int b = 0; b < kSpectrumBands; ++b) {
            const float fc = fMin * std::pow(ratio, static_cast<float>(b));
            const float lo = fc / halfStep, hi = fc * halfStep;
            const int centre = static_cast<int>(std::lround(fc / binHz));
            int b0 = std::min(static_cast<int>(std::floor(lo / binHz)), centre);
            int b1 = std::max(static_cast<int>(std::ceil(hi / binHz)), centre + 1);
            b0 = std::clamp(b0, 1, nyquistBin);
            b1 = std::clamp(b1, b0 + 1, nyquistBin + 1);
            binLo[b] = b0;
            binHi[b] = b1;
        }
    }

    // In-place radix-2 FFT. Replaces ~100 Goertzel filters (each a 1024-step
    // dependent chain) with ~5k butterflies.
    void Fft(float* re, float* im) const {
        for (size_t i = 0; i < kWindow; ++i) {
            const size_t j = bitRev[i];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (size_t len = 2; len <= kWindow; len <<= 1) {
            const size_t half = len >> 1, step = kWindow / len;
            for (size_t i = 0; i < kWindow; i += len) {
                for (size_t k = 0; k < half; ++k) {
                    const float wr = twCos[k * step], wi = twSin[k * step];
                    const size_t a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }
    }

    void Analyze() {
        float re[kWindow];
        float im[kWindow];
        for (size_t i = 0; i < kWindow; ++i) {
            re[i] = ring[(write + i) % kWindow] * window[i];
            im[i] = 0.0f;
        }
        Fft(re, im);
        float peak = 0.0f;
        float raw[kSpectrumBands];
        for (int b = 0; b < kSpectrumBands; ++b) {
            float sum = 0.0f, mx = 0.0f;
            for (int k = binLo[b]; k < binHi[b]; ++k) {
                const float p = re[k] * re[k] + im[k] * im[k];
                sum += p;
                mx = std::max(mx, p);
            }
            // Half mean, half strongest bin: broadband energy still counts, but a
            // single strong partial is not averaged away.
            const float power = 0.5f * (sum / static_cast<float>(binHi[b] - binLo[b])) + 0.5f * mx;
            // 4/kWindow normalises a Hann-windowed sinusoid back to ~its amplitude.
            float amp = std::sqrt(power) * (4.0f / kWindow);
            // Music has a steep spectral tilt; lift the highs so they stay visible.
            const float tilt = 1.0f + 2.2f * static_cast<float>(b) / (kSpectrumBands - 1);
            amp *= tilt;
            raw[b] = amp;
            peak = std::max(peak, amp);
        }
        agc = std::max(peak, agc * 0.9985f);
        const float norm = 1.0f / std::max(agc, 0.02f);
        for (int b = 0; b < kSpectrumBands; ++b) {
            float v = std::sqrt(Clamp(raw[b] * norm, 0.0f, 1.0f));
            // Silence must stay silent even though the auto-gain is boosted.
            v *= Clamp(peak * 40.0f, 0.0f, 1.0f);
            smooth[b] += (v > smooth[b] ? 0.65f : 0.14f) * (v - smooth[b]);
        }
    }

    void Decay() {
        for (int b = 0; b < kSpectrumBands; ++b) smooth[b] *= 0.88f;
    }

    // Returns true when at least one new analysis frame was produced.
    bool Feed(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
        if (!data || !frames || !format || !format->nChannels) { Decay(); return true; }
        const bool isFloat =
            format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
             IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                         kSubTypeIeeeFloat));
        const bool is16 = format->wBitsPerSample == 16;
        if (!isFloat && !is16) { Decay(); return true; }

        Configure(static_cast<float>(format->nSamplesPerSec));
        const UINT32 ch = format->nChannels;
        bool produced = false;
        for (UINT32 f = 0; f < frames; ++f) {
            float mono = 0.0f;
            if (isFloat) {
                const float* p = reinterpret_cast<const float*>(data) + static_cast<size_t>(f) * ch;
                for (UINT32 c = 0; c < ch; ++c) mono += p[c];
            } else {
                const int16_t* p = reinterpret_cast<const int16_t*>(data) + static_cast<size_t>(f) * ch;
                for (UINT32 c = 0; c < ch; ++c) mono += p[c] / 32768.0f;
            }
            ring[write] = mono / ch;
            write = (write + 1) % kWindow;
            if (++sinceHop >= kHop) {
                sinceHop = 0;
                Analyze();
                produced = true;
            }
        }
        return produced;
    }
};

SpectrumAnalyzer g_spectrumAnalyzer;

void PublishSpectrumBands() {
    std::lock_guard lock(g_stateMutex);
    for (int b = 0; b < kSpectrumBands; ++b) {
        g_state.bands[b] = g_spectrumAnalyzer.smooth[b];
    }
}

void DecayAndPublishSpectrum() {
    if (!g_spectrumBandsNeeded.load(std::memory_order_relaxed)) {
        return;
    }
    g_spectrumAnalyzer.Decay();
    std::lock_guard lock(g_stateMutex);
    for (int b = 0; b < kSpectrumBands; ++b) {
        g_state.bands[b] = g_spectrumAnalyzer.smooth[b];
    }
}

void PushWaveformSample(float amplitude) {
    std::lock_guard lock(g_stateMutex);

    float lastVal = 0.0f;
    if (g_state.waveformWrite > 0) {
        lastVal = g_state.waveform[(g_state.waveformWrite - 1) % g_state.waveform.size()];
    }

    // Apply an attack/release envelope (Exponential Moving Average)
    // Quick snappy attack (0.7) for beats, buttery smooth release (0.85) for decay.
    float smoothed;
    if (amplitude > lastVal) {
        smoothed = lastVal * 0.3f + amplitude * 0.7f;
    } else {
        smoothed = lastVal * 0.85f + amplitude * 0.15f;
    }

    g_state.waveform[g_state.waveformWrite % g_state.waveform.size()] = smoothed;
    ++g_state.waveformWrite;
}

void PushAudioChunks(BYTE* data, UINT32 frames, WAVEFORMATEX* format) {
    if (g_spectrumBandsNeeded.load(std::memory_order_relaxed) &&
        g_spectrumAnalyzer.Feed(data, frames, format)) {
        PublishSpectrumBands();
    }
    if (!data || !frames || !format || !format->nChannels) {
        PushWaveformSample(0.0f);
        return;
    }

    constexpr UINT32 chunkFrames = 64;
    const UINT32 channels = format->nChannels;
    const bool isFloat =
        format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat,
                     kSubTypeIeeeFloat));
    const bool is16 = format->wBitsPerSample == 16;

    if (!isFloat && !is16) {
        PushWaveformSample(SampleAudioAmplitude(data, frames, format));
        return;
    }

    for (UINT32 frame = 0; frame < frames; frame += chunkFrames) {
        const UINT32 chunk = std::min(chunkFrames, frames - frame);
        double sum = 0.0;
        const size_t samples = static_cast<size_t>(chunk) * channels;
        const size_t start = static_cast<size_t>(frame) * channels;

        if (isFloat) {
            auto* f = reinterpret_cast<float*>(data);
            for (size_t i = 0; i < samples; ++i) {
                const double v = f[start + i];
                sum += v * v;
            }
        } else {
            auto* s = reinterpret_cast<int16_t*>(data);
            for (size_t i = 0; i < samples; ++i) {
                const double v = s[start + i] / 32768.0;
                sum += v * v;
            }
        }

        PushWaveformSample(Clamp(static_cast<float>(std::sqrt(sum / samples) * 4.0), 0.0f, 1.0f));
    }
}

}


DWORD WINAPI AudioThreadProc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        // Don't touch the audio engine at all unless a playing waveform is
        // actually going to be shown right now. Cheap poll, no capture open.
        if (!g_audioCaptureNeeded.load(std::memory_order_relaxed)) {
            WaitForSingleObject(g_stopEvent, 250);
            continue;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mixFormat = nullptr;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(hr)) {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetMixFormat(&mixFormat);
        }
        if (SUCCEEDED(hr)) {
            REFERENCE_TIME bufferDuration = 10000000 / 5;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufferDuration, 0, mixFormat, nullptr);
        }
        if (SUCCEEDED(hr)) {
            hr = client->GetService(IID_PPV_ARGS(&capture));
        }
        if (SUCCEEDED(hr)) {
            hr = client->Start();
        }

        if (FAILED(hr)) {
            if (mixFormat) {
                CoTaskMemFree(mixFormat);
            }
            WaitForSingleObject(g_stopEvent, 2500);
            continue;
        }

        // Watchdog state — reset every time a capture session (re)starts so we
        // don't flag a stall before the first packet has even arrived.
        double lastPacketTime = NowSeconds();
        double nextWatchdogCheck = lastPacketTime + 1.0;

        while (WaitForSingleObject(g_stopEvent, 16) == WAIT_TIMEOUT) {
            if (!g_audioCaptureNeeded.load(std::memory_order_relaxed)) {
                break;
            }
            UINT32 packetFrames = 0;
            if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                break;
            }

            float amplitude = 0.0f;
            int packets = 0;
            while (packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                    break;
                }

                lastPacketTime = NowSeconds();

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    amplitude = std::max(amplitude, SampleAudioAmplitude(data, frames, mixFormat));
                    PushAudioChunks(data, frames, mixFormat);
                } else {
                    PushWaveformSample(0.0f);
                    DecayAndPublishSpectrum();
                }
                capture->ReleaseBuffer(frames);
                ++packets;

                if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
                    packetFrames = 0;
                }
            }

            if (packets > 0) {
                if (amplitude <= 0.001f) {
                    PushWaveformSample(0.0f);
                    DecayAndPublishSpectrum();
                }
            } else {
                PushWaveformSample(0.0f);
                DecayAndPublishSpectrum();
            }

            // --- Stall watchdog -------------------------------------------
            // Checked at most once a second (cheap: one time comparison plus
            // a mutex-protected bool read, same cost as work already done
            // elsewhere in this loop). Only forces a reconnect if SMTC says
            // media is playing but we've had zero packets for 5+ seconds —
            // that combination almost never happens unless the loopback
            // engine has stalled, so it won't fire during normal silence.
            const double now = NowSeconds();
            if (now >= nextWatchdogCheck) {
                nextWatchdogCheck = now + 1.0;
                bool mediaPlayingNow = false;
                {
                    std::lock_guard lock(g_stateMutex);
                    mediaPlayingNow = g_state.media.playing;
                }
                if (mediaPlayingNow && (now - lastPacketTime) > 5.0) {
                    host::log(L"Audio loopback capture appears stalled while media is playing; reconnecting...");
                    break;
                }
            }
        }

        client->Stop();
        if (mixFormat) {
            CoTaskMemFree(mixFormat);
        }
    }

    if (SUCCEEDED(hrCo)) {
        CoUninitialize();
    }

    return 0;
}


}
