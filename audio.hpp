// gm14 platform-agnostic audio engine.
//
// Semantics mirror GameMaker: Studio 1.4 as implemented by the official HTML5
// runtime (scripts/functions/Function_Sound.js + Function_Sound_Legacy.js) and
// ENIGMA's SDL audio system. Two id spaces coexist:
//
//   * asset index      : the SOND list index (0 .. sounds.size()-1). Functions
//                        taking an asset index operate on *all* voices playing
//                        that asset (legacy snd_*/sound_* behaviour).
//   * voice handle     : returned by audio_play_sound(), >= kHandleBase (300000).
//                        Functions taking a handle operate on that one voice.
//
// The mixer is intentionally separate from the VM so it can be driven by a
// deterministic NullBackend in tests, or an ALSA device on the host.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace gm14 {

// GM's first playable handle. Matches HTML5 BASE_SOUND_INDEX.
static constexpr int kHandleBase = 300000;

// ---------------- decoded PCM ----------------
struct AudioClip {
    int sample_rate = 0;
    int channels = 0;          // 1 or 2
    int bits = 0;              // source bits (8/16/24/32)
    std::vector<int16_t> pcm;  // interleaved, always converted to 16-bit signed
    double duration() const {
        if (sample_rate <= 0 || channels <= 0) return 0.0;
        return (double)(pcm.size() / (size_t)channels) / (double)sample_rate;
    }
};

// Decode a WAV (RIFF) blob into 16-bit interleaved PCM. Returns false on failure.
bool decode_wav(const uint8_t* data, size_t len, AudioClip& out);

// Decode an Ogg Vorbis blob into 16-bit interleaved PCM. Returns false on failure.
bool decode_ogg(const uint8_t* data, size_t len, AudioClip& out);

// Decode any supported container (RIFF/WAV or OggS/Ogg Vorbis).
bool decode_audio(const uint8_t* data, size_t len, AudioClip& out);

// ---------------- output backend ----------------
class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual bool open(int sample_rate, int channels) = 0;
    virtual void close() = 0;
    // Queue interleaved 16-bit samples; block until space or drop.
    virtual void submit(const int16_t* samples, size_t count) = 0;
    // Number of frames consumed since open (used to drive the null clock).
    virtual uint64_t frames_played() const = 0;
    virtual bool real_device() const = 0;
};

std::unique_ptr<AudioBackend> make_null_backend();
std::unique_ptr<AudioBackend> make_alsa_backend(const char* device = nullptr);
std::unique_ptr<AudioBackend> make_waveout_backend();
// Best real-device backend for the current platform (ndsp on 3DS, waveOut on Windows, ALSA on host).
std::unique_ptr<AudioBackend> make_device_backend();

// ---------------- mixer ----------------
struct Voice {
    bool active = false;
    bool paused = false;
    bool loop = false;
    int asset = -1;                 // SOND index
    int handle = 0;                 // >= kHandleBase
    double position = 0.0;          // playback position in frames
    double gain = 1.0;
    double pitch = 1.0;             // 1.0 == normal
    double priority = 1.0;
    // Resampling state (fractional read offset).
    double cursor = 0.0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Register a decoded clip for an asset index. Voices read from here.
    void set_clip(int asset, std::shared_ptr<AudioClip> clip);
    bool has_clip(int asset) const;

    // Lazy clip loading with a byte budget. When set, the mixer will call
    // `loader(asset)` to decode a clip the first time it is needed and evict
    // the least-recently-used, non-playing clip when `budget_bytes` is exceeded.
    // This keeps memory bounded on consoles (decoding all 442 Undertale sounds
    // eagerly needs tens of MB). Pass nullptr to disable lazy loading.
    using ClipLoader = std::function<std::shared_ptr<AudioClip>(int asset)>;
    void set_clip_loader(ClipLoader loader, size_t budget_bytes);
    // Ensure `asset` is resident (loads it if a loader is set). Returns pointer
    // or nullptr. Marks the clip as recently used.
    const AudioClip* acquire(int asset);

    // Playback (audio_play_sound). Returns a handle >= kHandleBase, or -1.
    int play(int asset, bool loop, double gain, double offset_seconds, double pitch, double priority);
    // Stop every voice playing `asset` (legacy sound_stop / audio_stop_sound(asset)).
    void stop_asset(int asset);
    // Stop one voice by handle.
    void stop_handle(int handle);
    void stop_all();

    void pause_asset(int asset);
    void pause_handle(int handle);
    void resume_asset(int asset);
    void resume_handle(int handle);
    void pause_all();
    void resume_all();

    // Gain: asset applies to all its voices; handle to one.
    void set_gain_asset(int asset, double gain);
    void set_gain_handle(int handle, double gain);
    double get_gain_asset(int asset) const;
    double get_gain_handle(int handle) const;

    void set_pitch_asset(int asset, double pitch);
    void set_pitch_handle(int handle, double pitch);
    double get_pitch_asset(int asset) const;
    double get_pitch_handle(int handle) const;

    // Track position (seconds).
    double get_track_position(int id) const;
    void set_track_position(int handle, double seconds);

    bool is_playing(int id) const;      // id may be asset (<handle) or handle
    bool is_paused(int id) const;

    void set_master_gain(double g);
    double master_gain() const { return m_master_gain; }
    void set_max_voices(int n);

    // Advance and render `frames` output frames. In tests, call this manually;
    // on the host a background thread calls it. Deterministic.
    void render(int frames);

    // Backend access (tests use NullBackend and advance via render()).
    AudioBackend& backend() { return *m_backend; }
    void set_backend(std::unique_ptr<AudioBackend> b);

    int sample_rate() const { return m_sample_rate; }
    int channels() const { return m_channels; }

    size_t resident_bytes() const { return m_resident_bytes; }
    size_t budget_bytes() const { return m_budget_bytes; }

    // Flush accumulated samples to the backend.
    void flush();

private:
    Voice* find_free_voice();
    Voice* voice_for_handle(int handle);
    void mix_voice(Voice& v, int frames);
    void evict_if_needed(int protect_asset = -1);

    std::vector<Voice> m_voices;
    std::vector<std::shared_ptr<AudioClip>> m_clips;  // by asset index
    std::vector<uint64_t> m_clip_used;                // LRU stamp per clip
    ClipLoader m_loader;
    size_t m_budget_bytes = 0;
    size_t m_resident_bytes = 0;
    uint64_t m_use_counter = 0;
    std::unique_ptr<AudioBackend> m_backend;
    std::vector<int16_t> m_mix;                       // interleaved scratch
    double m_master_gain = 1.0;
    int m_sample_rate = 44100;
    int m_channels = 2;
    int m_next_handle = kHandleBase;
    int m_max_voices = 32;
};

} // namespace gm14
