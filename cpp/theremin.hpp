// theremin.hpp - Tiny digital theremin (sine-wave synth) with platform
// audio backends. Header-only.
//
// Backends, picked at compile time:
//   * Linux:   ALSA  (link with -lasound)        -DOSC_AUDIO_ALSA
//   * macOS:   CoreAudio AudioQueue              -DOSC_AUDIO_COREAUDIO
//   * Windows: WinMM  (link with -lwinmm)        -DOSC_AUDIO_WINMM
//   * Fallback: silent (no audio output)         -DOSC_AUDIO_NONE
//
// The constructor opens the audio device synchronously and only spawns
// a worker thread for streaming if the open succeeded. That means
// `available()` and `backend()` are correct the moment the constructor
// returns -- the UI never has to race with audio initialization.

#pragma once

#include <atomic>
#include <chrono>
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
    Theremin(int sample_rate = 44100, float max_amp = 0.5f)
        : sample_rate_(sample_rate), max_amp_(max_amp) {
        // Open the audio device synchronously. open_device() sets
        // backend_ either to a description ("ALSA 44100 Hz") or to a
        // "silent (...)" diagnostic. If it succeeded, mark ourselves
        // available and start the streaming worker.
        if (open_device()) {
            available_ = true;
            worker_ = std::thread([this]{ this->stream_loop(); });
        }
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
            close_device();
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

    bool open_device();    // platform-specific
    void stream_loop();    // platform-specific
    void close_device();   // platform-specific

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

    // Platform-specific device state.
#if defined(OSC_AUDIO_ALSA)
    snd_pcm_t* alsa_pcm_ = nullptr;
#elif defined(OSC_AUDIO_COREAUDIO)
    AudioQueueRef       ca_queue_ = nullptr;
    AudioQueueBufferRef ca_bufs_[3] = {nullptr, nullptr, nullptr};
#elif defined(OSC_AUDIO_WINMM)
    HWAVEOUT wm_hw_ = nullptr;
    static constexpr int kWmFrames = 512;
    static constexpr int kWmQueue  = 4;
    std::vector<float> wm_data_;        // sized kWmFrames * kWmQueue
    WAVEHDR wm_hdr_[kWmQueue]{};
#endif
};

// ---------------- ALSA -----------------------------------------------------
#if defined(OSC_AUDIO_ALSA)
inline bool Theremin::open_device() {
    int err = snd_pcm_open(&alsa_pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        backend_ = std::string("silent (ALSA open failed: ") +
                   snd_strerror(err) + ")";
        alsa_pcm_ = nullptr;
        return false;
    }
    err = snd_pcm_set_params(alsa_pcm_,
                             SND_PCM_FORMAT_FLOAT_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             1,                  // channels
                             sample_rate_,
                             1,                  // soft resample
                             100000);            // ~100ms latency
    if (err < 0) {
        backend_ = std::string("silent (ALSA set_params: ") +
                   snd_strerror(err) + ")";
        snd_pcm_close(alsa_pcm_);
        alsa_pcm_ = nullptr;
        return false;
    }
    backend_ = "ALSA " + std::to_string(sample_rate_) + " Hz";
    return true;
}

inline void Theremin::stream_loop() {
    constexpr int kFrames = 512;
    std::vector<float> buf(kFrames);
    while (running_.load()) {
        fill(buf.data(), kFrames);
        snd_pcm_sframes_t n = snd_pcm_writei(alsa_pcm_, buf.data(), kFrames);
        if (n < 0) snd_pcm_recover(alsa_pcm_, static_cast<int>(n), 1);
    }
}

inline void Theremin::close_device() {
    if (alsa_pcm_) {
        snd_pcm_drain(alsa_pcm_);
        snd_pcm_close(alsa_pcm_);
        alsa_pcm_ = nullptr;
    }
}

// ---------------- CoreAudio (macOS) ---------------------------------------
#elif defined(OSC_AUDIO_COREAUDIO)
inline bool Theremin::open_device() {
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

    auto cb = [](void* user, AudioQueueRef qq, AudioQueueBufferRef b) {
        auto* self = static_cast<Theremin*>(user);
        int frames = b->mAudioDataBytesCapacity / sizeof(float);
        self->fill(static_cast<float*>(b->mAudioData), frames);
        b->mAudioDataByteSize = frames * sizeof(float);
        if (self->running_.load()) AudioQueueEnqueueBuffer(qq, b, 0, nullptr);
    };

    if (AudioQueueNewOutput(&fmt, cb, this, nullptr, nullptr, 0, &ca_queue_) != 0) {
        backend_ = "silent (CoreAudio AudioQueueNewOutput failed)";
        ca_queue_ = nullptr;
        return false;
    }
    for (auto& b : ca_bufs_) {
        AudioQueueAllocateBuffer(ca_queue_, 512 * sizeof(float), &b);
        b->mAudioDataByteSize = 512 * sizeof(float);
        std::memset(b->mAudioData, 0, b->mAudioDataByteSize);
        AudioQueueEnqueueBuffer(ca_queue_, b, 0, nullptr);
    }
    AudioQueueStart(ca_queue_, nullptr);
    backend_ = "CoreAudio " + std::to_string(sample_rate_) + " Hz";
    return true;
}

inline void Theremin::stream_loop() {
    // CoreAudio's AudioQueue runs the audio callback on its own thread;
    // this worker just keeps the streaming context alive until stop().
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

inline void Theremin::close_device() {
    if (ca_queue_) {
        AudioQueueStop(ca_queue_, true);
        AudioQueueDispose(ca_queue_, true);
        ca_queue_ = nullptr;
        for (auto& b : ca_bufs_) b = nullptr;
    }
}

// ---------------- WinMM (Windows) -----------------------------------------
#elif defined(OSC_AUDIO_WINMM)
inline bool Theremin::open_device() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = sample_rate_;
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = 4;
    fmt.nAvgBytesPerSec = sample_rate_ * 4;
    if (waveOutOpen(&wm_hw_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL)
            != MMSYSERR_NOERROR) {
        backend_ = "silent (waveOutOpen failed)";
        wm_hw_ = nullptr;
        return false;
    }
    wm_data_.assign(static_cast<size_t>(kWmFrames) * kWmQueue, 0.0f);
    for (int i = 0; i < kWmQueue; ++i) {
        wm_hdr_[i] = WAVEHDR{};
        wm_hdr_[i].lpData         = reinterpret_cast<LPSTR>(
                                        wm_data_.data() + i * kWmFrames);
        wm_hdr_[i].dwBufferLength = kWmFrames * sizeof(float);
        waveOutPrepareHeader(wm_hw_, &wm_hdr_[i], sizeof(WAVEHDR));
    }
    backend_ = "WinMM " + std::to_string(sample_rate_) + " Hz";
    return true;
}

inline void Theremin::stream_loop() {
    int idx = 0;
    while (running_.load()) {
        WAVEHDR& h = wm_hdr_[idx];
        while (running_.load() && (h.dwFlags & WHDR_INQUEUE)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!running_.load()) break;
        fill(reinterpret_cast<float*>(h.lpData), kWmFrames);
        h.dwFlags &= ~WHDR_DONE;
        waveOutWrite(wm_hw_, &h, sizeof(WAVEHDR));
        idx = (idx + 1) % kWmQueue;
    }
}

inline void Theremin::close_device() {
    if (wm_hw_) {
        waveOutReset(wm_hw_);
        for (int i = 0; i < kWmQueue; ++i) {
            waveOutUnprepareHeader(wm_hw_, &wm_hdr_[i], sizeof(WAVEHDR));
        }
        waveOutClose(wm_hw_);
        wm_hw_ = nullptr;
    }
}

// ---------------- Silent fallback -----------------------------------------
#else  // OSC_AUDIO_NONE
inline bool Theremin::open_device() {
    backend_ = "silent (no audio backend compiled in)";
    return false;
}
inline void Theremin::stream_loop() { /* never started */ }
inline void Theremin::close_device() {}
#endif

} // namespace osc
