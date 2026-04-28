// theremin.hpp - Tiny digital theremin (sine-wave synth) with platform
// audio backends. Header-only.
//
// Backends, picked at compile time:
//   * Linux:   ALSA  (link with -lasound)        -DOSC_AUDIO_ALSA
//   * macOS:   CoreAudio AudioQueue              -DOSC_AUDIO_COREAUDIO
//   * Windows: WinMM  (link with -lwinmm)        -DOSC_AUDIO_WINMM
//   * Fallback: silent (no audio output)         -DOSC_AUDIO_NONE
//
// If you don't define one explicitly, the header auto-picks based on the
// platform and (on Linux) whether <alsa/asoundlib.h> is available. See
// the Makefile for the link flags.
//
// Usage:
//   osc::Theremin therm;            // starts the audio thread
//   therm.set_pitch(440.0f);
//   therm.set_volume(0.5f);
//   therm.set_on(true);
//   ...
//   therm.stop();                   // optional; destructor also stops it

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---- Backend auto-detection ----------------------------------------------
#if !defined(OSC_AUDIO_ALSA) && !defined(OSC_AUDIO_COREAUDIO) && \
    !defined(OSC_AUDIO_WINMM) && !defined(OSC_AUDIO_NONE)
    #if defined(_WIN32)
        #define OSC_AUDIO_WINMM
    #elif defined(__APPLE__)
        #define OSC_AUDIO_COREAUDIO
    #elif defined(__linux__) && defined(__has_include)
        #if __has_include(<alsa/asoundlib.h>)
            #define OSC_AUDIO_ALSA
        #else
            #define OSC_AUDIO_NONE
        #endif
    #else
        #define OSC_AUDIO_NONE
    #endif
#endif

#if defined(OSC_AUDIO_ALSA)
    #include <alsa/asoundlib.h>
#elif defined(OSC_AUDIO_COREAUDIO)
    #include <AudioToolbox/AudioToolbox.h>
#elif defined(OSC_AUDIO_WINMM)
    #include <windows.h>
    #include <mmsystem.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "winmm.lib")
    #endif
#endif

namespace osc {

class Theremin {
public:
    Theremin(int sample_rate = 44100, float max_amp = 0.25f)
        : sample_rate_(sample_rate), max_amp_(max_amp) {
        worker_ = std::thread([this]{ this->run(); });
    }

    ~Theremin() { stop(); }

    void set_pitch(float hz)       { pitch_.store(clamp(hz, 20.0f, 20000.0f)); }
    void set_volume(float v)       { volume_.store(clamp(v, 0.0f, 1.0f)); }
    void set_on(bool on)           { on_.store(on); }

    float pitch()  const { return pitch_.load(); }
    float volume() const { return volume_.load(); }
    bool  on()     const { return on_.load(); }

    bool   available() const { return available_; }
    const std::string& backend() const { return backend_; }

    void stop() {
        if (running_.exchange(false)) {
            if (worker_.joinable()) worker_.join();
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Fill `buf` with `frames` mono float32 samples of sine wave at the
    // current pitch/volume. Maintains phase so unmuting doesn't click.
    void fill(float* buf, int frames) {
        const float twopi = 6.28318530717958647692f;
        float p = pitch_.load();
        float v = on_.load() ? (volume_.load() * max_amp_) : 0.0f;
        float inc = twopi * p / static_cast<float>(sample_rate_);
        for (int i = 0; i < frames; ++i) {
            buf[i] = std::sin(phase_) * v;
            phase_ += inc;
            if (phase_ >= twopi) phase_ -= twopi;
        }
    }

    void run();    // platform-specific, defined below

    int   sample_rate_;
    float max_amp_;
    std::atomic<float> pitch_  {440.0f};
    std::atomic<float> volume_ {0.5f};
    std::atomic<bool>  on_     {false};
    std::atomic<bool>  running_{true};
    bool  available_ = false;
    std::string backend_ = "silent";
    float phase_ = 0.0f;
    std::thread worker_;
};

// ---------------- ALSA -----------------------------------------------------
#if defined(OSC_AUDIO_ALSA)
inline void Theremin::run() {
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        backend_ = std::string("silent (ALSA open failed: ") +
                   snd_strerror(err) + ")";
        return;
    }
    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_FLOAT_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             1,                        // channels
                             sample_rate_,
                             1,                        // soft resample
                             100000);                  // ~100ms latency
    if (err < 0) {
        backend_ = std::string("silent (ALSA set_params: ") +
                   snd_strerror(err) + ")";
        snd_pcm_close(pcm);
        return;
    }
    available_ = true;
    backend_   = "ALSA " + std::to_string(sample_rate_) + " Hz";

    constexpr int kFrames = 512;
    std::vector<float> buf(kFrames);
    while (running_.load()) {
        fill(buf.data(), kFrames);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf.data(), kFrames);
        if (n < 0) snd_pcm_recover(pcm, static_cast<int>(n), 1);
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
}

// ---------------- CoreAudio (macOS) ---------------------------------------
#elif defined(OSC_AUDIO_COREAUDIO)
inline void Theremin::run() {
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = sample_rate_;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat |
                            kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = 4;
    fmt.mBytesPerPacket   = 4;

    AudioQueueRef q = nullptr;
    auto cb = [](void* user, AudioQueueRef qq, AudioQueueBufferRef b) {
        auto* self = static_cast<Theremin*>(user);
        int frames = b->mAudioDataBytesCapacity / sizeof(float);
        self->fill(static_cast<float*>(b->mAudioData), frames);
        b->mAudioDataByteSize = frames * sizeof(float);
        if (self->running_.load()) AudioQueueEnqueueBuffer(qq, b, 0, nullptr);
    };
    if (AudioQueueNewOutput(&fmt, cb, this, nullptr, nullptr, 0, &q) != 0) {
        backend_ = "silent (CoreAudio AudioQueueNewOutput failed)";
        return;
    }
    AudioQueueBufferRef bufs[3] = {nullptr, nullptr, nullptr};
    for (auto& b : bufs) {
        AudioQueueAllocateBuffer(q, 512 * sizeof(float), &b);
        b->mAudioDataByteSize = 512 * sizeof(float);
        std::memset(b->mAudioData, 0, b->mAudioDataByteSize);
        AudioQueueEnqueueBuffer(q, b, 0, nullptr);
    }
    AudioQueueStart(q, nullptr);
    available_ = true;
    backend_   = "CoreAudio " + std::to_string(sample_rate_) + " Hz";

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    AudioQueueStop(q, true);
    AudioQueueDispose(q, true);
}

// ---------------- WinMM (Windows) -----------------------------------------
#elif defined(OSC_AUDIO_WINMM)
inline void Theremin::run() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag     = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels      = 1;
    fmt.nSamplesPerSec = sample_rate_;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign    = 4;
    fmt.nAvgBytesPerSec= sample_rate_ * 4;
    HWAVEOUT hw = nullptr;
    if (waveOutOpen(&hw, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        backend_ = "silent (waveOutOpen failed)";
        return;
    }
    available_ = true;
    backend_   = "WinMM " + std::to_string(sample_rate_) + " Hz";

    constexpr int kFrames = 512;
    constexpr int kQueue  = 4;
    std::vector<float> data(kFrames * kQueue);
    WAVEHDR hdr[kQueue]{};
    for (int i = 0; i < kQueue; ++i) {
        hdr[i].lpData         = reinterpret_cast<LPSTR>(data.data() + i * kFrames);
        hdr[i].dwBufferLength = kFrames * sizeof(float);
        waveOutPrepareHeader(hw, &hdr[i], sizeof(WAVEHDR));
    }
    int idx = 0;
    while (running_.load()) {
        WAVEHDR& h = hdr[idx];
        // Wait for this slot to be free
        while (running_.load() && (h.dwFlags & WHDR_INQUEUE)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!running_.load()) break;
        fill(reinterpret_cast<float*>(h.lpData), kFrames);
        h.dwFlags &= ~WHDR_DONE;
        waveOutWrite(hw, &h, sizeof(WAVEHDR));
        idx = (idx + 1) % kQueue;
    }
    waveOutReset(hw);
    for (int i = 0; i < kQueue; ++i) waveOutUnprepareHeader(hw, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(hw);
}

// ---------------- Silent fallback -----------------------------------------
#else  // OSC_AUDIO_NONE
inline void Theremin::run() {
    backend_ = "silent (no audio backend compiled in)";
    while (running_.load()) {
        // Pretend to consume samples so phase advances naturally.
        float scratch[256];
        fill(scratch, 256);
        std::this_thread::sleep_for(std::chrono::milliseconds(
            256 * 1000 / sample_rate_));
    }
}
#endif

} // namespace osc
