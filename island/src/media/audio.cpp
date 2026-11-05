#include "media/audio.h"
#include "core/model.h"
#include "core/state.h"
#include "core/util.h"

namespace island
{
namespace
{
float sample_audio_amplitude(BYTE* data, UINT32 frames, WAVEFORMATEX* format)
{
    if (!data || !frames || !format || !format->nChannels)
    {
        return 0.0f;
    }

    double sum = 0.0;
    std::size_t samples = static_cast<std::size_t>(frames) * format->nChannels;

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat, sub_type_ieee_float)))
    {
        auto* f = reinterpret_cast<float*>(data);
        for (std::size_t i = 0; i < samples; ++i)
        {
            sum += f[i] * f[i];
        }
    }
    else if (format->wBitsPerSample == 16)
    {
        auto* s = reinterpret_cast<std::int16_t*>(data);
        for (std::size_t i = 0; i < samples; ++i)
        {
            const double v = s[i] / 32768.0;
            sum += v * v;
        }
    }

    const double rms = samples ? std::sqrt(sum / samples) : 0.0;
    return clamp(static_cast<float>(rms * 4.0), 0.0f, 1.0f);
}

// FFT filter bank over a sliding window of the mixed-down system audio: spectrum_bands log-spaced bands
// (50 Hz .. 14 kHz) with attack/release and slow auto-gain. Audio thread only.
struct SpectrumAnalyzer
{
    static constexpr std::size_t window_samples = 1024;
    static constexpr std::size_t hop = 512;

    static constexpr int log2_window = 10;

    float ring[window_samples]{};
    float window[window_samples]{};
    float tw_cos[window_samples / 2]{};
    float tw_sin[window_samples / 2]{};
    std::uint16_t bit_rev[window_samples]{};
    int bin_lo[spectrum_bands]{}; // FFT bins covered by each band, [lo, hi)
    int bin_hi[spectrum_bands]{};
    float smooth[spectrum_bands]{};
    float agc = 0.04f;
    float sample_rate = 0.0f;
    std::size_t write = 0;
    std::size_t since_hop = 0;
    bool window_ready = false;

    void configure(float rate)
    {
        if (rate < 8000.0f)
        {
            rate = 48000.0f;
        }
        if (!window_ready)
        {
            for (std::size_t i = 0; i < window_samples; ++i)
            {
                window[i] = 0.5f - 0.5f * std::cos(6.2831853f * i / (window_samples - 1));
            }
            for (std::size_t i = 0; i < window_samples / 2; ++i)
            {
                const float a = -6.2831853f * static_cast<float>(i) / static_cast<float>(window_samples);
                tw_cos[i] = std::cos(a);
                tw_sin[i] = std::sin(a);
            }
            for (std::size_t i = 0; i < window_samples; ++i)
            {
                std::size_t r = 0;
                for (int bit = 0; bit < log2_window; ++bit)
                {
                    if (i & (static_cast<std::size_t>(1) << bit))
                    {
                        r |= static_cast<std::size_t>(1) << (log2_window - 1 - bit);
                    }
                }
                bit_rev[i] = static_cast<std::uint16_t>(r);
            }
            window_ready = true;
        }
        if (rate == sample_rate)
        {
            return;
        }
        sample_rate = rate;
        const float f_min = 50.0f;
        const float f_max = std::min(14000.0f, rate * 0.45f);
        const float ratio = std::pow(f_max / f_min, 1.0f / (spectrum_bands - 1));
        const float half_step = std::sqrt(ratio);
        const float bin_hz = rate / window_samples;
        const int nyquist_bin = static_cast<int>(window_samples / 2) - 1;
        for (int b = 0; b < spectrum_bands; ++b)
        {
            const float fc = f_min * std::pow(ratio, static_cast<float>(b));
            const float lo = fc / half_step, hi = fc * half_step;
            const int centre = static_cast<int>(std::lround(fc / bin_hz));
            int b0 = std::min(static_cast<int>(std::floor(lo / bin_hz)), centre);
            int b1 = std::max(static_cast<int>(std::ceil(hi / bin_hz)), centre + 1);
            b0 = std::clamp(b0, 1, nyquist_bin);
            b1 = std::clamp(b1, b0 + 1, nyquist_bin + 1);
            bin_lo[b] = b0;
            bin_hi[b] = b1;
        }
    }

    void fft(float* re, float* im) const
    {
        for (std::size_t i = 0; i < window_samples; ++i)
        {
            const std::size_t j = bit_rev[i];
            if (j > i)
            {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (std::size_t len = 2; len <= window_samples; len <<= 1)
        {
            const std::size_t half = len >> 1, step = window_samples / len;
            for (std::size_t i = 0; i < window_samples; i += len)
            {
                for (std::size_t k = 0; k < half; ++k)
                {
                    const float wr = tw_cos[k * step], wi = tw_sin[k * step];
                    const std::size_t a = i + k, b = a + half;
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

    void analyze()
    {
        float re[window_samples];
        float im[window_samples];
        for (std::size_t i = 0; i < window_samples; ++i)
        {
            re[i] = ring[(write + i) % window_samples] * window[i];
            im[i] = 0.0f;
        }
        fft(re, im);
        float peak = 0.0f;
        float raw[spectrum_bands];
        for (int b = 0; b < spectrum_bands; ++b)
        {
            float sum = 0.0f, mx = 0.0f;
            for (int k = bin_lo[b]; k < bin_hi[b]; ++k)
            {
                const float p = re[k] * re[k] + im[k] * im[k];
                sum += p;
                mx = std::max(mx, p);
            }
            const float power = 0.5f * (sum / static_cast<float>(bin_hi[b] - bin_lo[b])) + 0.5f * mx;
            float amp = std::sqrt(power) * (4.0f / window_samples);
            // Music has a steep spectral tilt; lift the highs so they stay visible.
            const float tilt = 1.0f + 2.2f * static_cast<float>(b) / (spectrum_bands - 1);
            amp *= tilt;
            raw[b] = amp;
            peak = std::max(peak, amp);
        }
        agc = std::max(peak, agc * 0.9985f);
        const float norm = 1.0f / std::max(agc, 0.02f);
        for (int b = 0; b < spectrum_bands; ++b)
        {
            float v = std::sqrt(clamp(raw[b] * norm, 0.0f, 1.0f));
            // Silence must stay silent even though the auto-gain is boosted.
            v *= clamp(peak * 40.0f, 0.0f, 1.0f);
            smooth[b] += (v > smooth[b] ? 0.65f : 0.14f) * (v - smooth[b]);
        }
    }

    void decay()
    {
        for (int b = 0; b < spectrum_bands; ++b)
        {
            smooth[b] *= 0.88f;
        }
    }

    bool feed(BYTE* data, UINT32 frames, WAVEFORMATEX* format)
    {
        if (!data || !frames || !format || !format->nChannels)
        {
            decay();
            return true;
        }
        const bool is_float =
            format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
             IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat, sub_type_ieee_float));
        const bool is16 = format->wBitsPerSample == 16;
        if (!is_float && !is16)
        {
            decay();
            return true;
        }

        configure(static_cast<float>(format->nSamplesPerSec));
        const UINT32 ch = format->nChannels;
        bool produced = false;
        for (UINT32 f = 0; f < frames; ++f)
        {
            float mono = 0.0f;
            if (is_float)
            {
                const float* p = reinterpret_cast<const float*>(data) + static_cast<std::size_t>(f) * ch;
                for (UINT32 c = 0; c < ch; ++c)
                {
                    mono += p[c];
                }
            }
            else
            {
                const std::int16_t* p = reinterpret_cast<const std::int16_t*>(data) + static_cast<std::size_t>(f) * ch;
                for (UINT32 c = 0; c < ch; ++c)
                {
                    mono += p[c] / 32768.0f;
                }
            }
            ring[write] = mono / ch;
            write = (write + 1) % window_samples;
            if (++since_hop >= hop)
            {
                since_hop = 0;
                analyze();
                produced = true;
            }
        }
        return produced;
    }
};

SpectrumAnalyzer g_spectrum_analyzer;

void publish_spectrum_bands()
{
    std::lock_guard lock(g_state_mutex);
    for (int b = 0; b < spectrum_bands; ++b)
    {
        g_state.bands[b] = g_spectrum_analyzer.smooth[b];
    }
}

void decay_and_publish_spectrum()
{
    if (!g_spectrum_bands_needed.load(std::memory_order_relaxed))
    {
        return;
    }
    g_spectrum_analyzer.decay();
    std::lock_guard lock(g_state_mutex);
    for (int b = 0; b < spectrum_bands; ++b)
    {
        g_state.bands[b] = g_spectrum_analyzer.smooth[b];
    }
}

void push_waveform_sample(float amplitude)
{
    std::lock_guard lock(g_state_mutex);

    float last_val = 0.0f;
    if (g_state.waveform_write > 0)
    {
        last_val = g_state.waveform[(g_state.waveform_write - 1) % g_state.waveform.size()];
    }

    float smoothed;
    if (amplitude > last_val)
    {
        smoothed = last_val * 0.3f + amplitude * 0.7f;
    }
    else
    {
        smoothed = last_val * 0.85f + amplitude * 0.15f;
    }

    g_state.waveform[g_state.waveform_write % g_state.waveform.size()] = smoothed;
    ++g_state.waveform_write;
}

void push_audio_chunks(BYTE* data, UINT32 frames, WAVEFORMATEX* format)
{
    if (g_spectrum_bands_needed.load(std::memory_order_relaxed) && g_spectrum_analyzer.feed(data, frames, format))
    {
        publish_spectrum_bands();
    }
    if (!data || !frames || !format || !format->nChannels)
    {
        push_waveform_sample(0.0f);
        return;
    }

    constexpr UINT32 chunk_frames = 64;
    const UINT32 channels = format->nChannels;
    const bool is_float =
        format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
         IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat, sub_type_ieee_float));
    const bool is16 = format->wBitsPerSample == 16;

    if (!is_float && !is16)
    {
        push_waveform_sample(sample_audio_amplitude(data, frames, format));
        return;
    }

    for (UINT32 frame = 0; frame < frames; frame += chunk_frames)
    {
        const UINT32 chunk = std::min(chunk_frames, frames - frame);
        double sum = 0.0;
        const std::size_t samples = static_cast<std::size_t>(chunk) * channels;
        const std::size_t start = static_cast<std::size_t>(frame) * channels;

        if (is_float)
        {
            auto* f = reinterpret_cast<float*>(data);
            for (std::size_t i = 0; i < samples; ++i)
            {
                const double v = f[start + i];
                sum += v * v;
            }
        }
        else
        {
            auto* s = reinterpret_cast<std::int16_t*>(data);
            for (std::size_t i = 0; i < samples; ++i)
            {
                const double v = s[start + i] / 32768.0;
                sum += v * v;
            }
        }

        push_waveform_sample(clamp(static_cast<float>(std::sqrt(sum / samples) * 4.0), 0.0f, 1.0f));
    }
}

}

DWORD WINAPI audio_thread_proc(void*)
{
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
    {
        // The audio engine is untouched unless a visualizer that needs audio is on screen.
        if (!g_audio_capture_needed.load(std::memory_order_relaxed))
        {
            WaitForSingleObject(g_stop_event, 250);
            continue;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mix_format = nullptr;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr))
        {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (SUCCEEDED(hr))
        {
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
        }
        if (SUCCEEDED(hr))
        {
            hr = client->GetMixFormat(&mix_format);
        }
        if (SUCCEEDED(hr))
        {
            REFERENCE_TIME buffer_duration = 10000000 / 5;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, buffer_duration, 0,
                                    mix_format, nullptr);
        }
        if (SUCCEEDED(hr))
        {
            hr = client->GetService(IID_PPV_ARGS(&capture));
        }
        if (SUCCEEDED(hr))
        {
            hr = client->Start();
        }

        if (FAILED(hr))
        {
            if (mix_format)
            {
                CoTaskMemFree(mix_format);
            }
            WaitForSingleObject(g_stop_event, 2500);
            continue;
        }

        double last_packet_time = now_seconds();
        double next_watchdog_check = last_packet_time + 1.0;

        while (WaitForSingleObject(g_stop_event, 16) == WAIT_TIMEOUT)
        {
            if (!g_audio_capture_needed.load(std::memory_order_relaxed))
            {
                break;
            }
            UINT32 packet_frames = 0;
            if (FAILED(capture->GetNextPacketSize(&packet_frames)))
            {
                break;
            }

            float amplitude = 0.0f;
            int packets = 0;
            while (packet_frames > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                {
                    break;
                }

                last_packet_time = now_seconds();

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                {
                    amplitude = std::max(amplitude, sample_audio_amplitude(data, frames, mix_format));
                    push_audio_chunks(data, frames, mix_format);
                }
                else
                {
                    push_waveform_sample(0.0f);
                    decay_and_publish_spectrum();
                }
                capture->ReleaseBuffer(frames);
                ++packets;

                if (FAILED(capture->GetNextPacketSize(&packet_frames)))
                {
                    packet_frames = 0;
                }
            }

            if (packets > 0)
            {
                if (amplitude <= 0.001f)
                {
                    push_waveform_sample(0.0f);
                    decay_and_publish_spectrum();
                }
            }
            else
            {
                push_waveform_sample(0.0f);
                decay_and_publish_spectrum();
            }

            // Stall watchdog (once a second): reconnect only when SMTC says playing but no packet came for 5 s.
            const double now = now_seconds();
            if (now >= next_watchdog_check)
            {
                next_watchdog_check = now + 1.0;
                bool media_playing_now = false;
                {
                    std::lock_guard lock(g_state_mutex);
                    media_playing_now = g_state.media.playing;
                }
                if (media_playing_now && (now - last_packet_time) > 5.0)
                {
                    host::log(L"Audio loopback capture appears stalled while media is playing; reconnecting...");
                    break;
                }
            }
        }

        client->Stop();
        if (mix_format)
        {
            CoTaskMemFree(mix_format);
        }
    }

    if (SUCCEEDED(hr_co))
    {
        CoUninitialize();
    }

    return 0;
}

}
