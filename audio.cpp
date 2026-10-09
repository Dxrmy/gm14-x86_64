// gm14 platform-agnostic audio engine implementation.
#include "audio.hpp"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

#if !defined(GM14_NO_ALSA) && !defined(__3DS__) && !defined(_WIN32)
#include <alsa/asoundlib.h>
#define GM14_HAVE_ALSA 1
#endif

#if defined(_WIN32) && !defined(GM14_NO_WAVEOUT)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#define GM14_HAVE_WAVEOUT 1
#endif

#if defined(__3DS__) && !defined(GM14_NO_NDSP)
#include <3ds.h>
#define GM14_HAVE_NDSP 1
#endif

// stb_vorbis: single-file Ogg Vorbis decoder (public domain). Used for the
// embedded .ogg sounds and, later, streamed music. Header-only inclusion keeps
// the build dependency-free on host and 3DS alike.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace gm14 {

// =====================================================================
// WAV decode
// =====================================================================
static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool decode_wav(const uint8_t* data, size_t len, AudioClip& out) {
    if (len < 44) return false;
    if (std::memcmp(data, "RIFF", 4) != 0) return false;
    if (std::memcmp(data + 8, "WAVE", 4) != 0) return false;

    int fmt = 0, channels = 0, bits = 0, sample_rate = 0;
    size_t data_off = 0, data_len = 0;

    size_t p = 12;
    while (p + 8 <= len) {
        const uint8_t* id = data + p;
        uint32_t csz = rd_u32(data + p + 4);
        size_t body = p + 8;
        if (csz > len - body) csz = (uint32_t)(len - body);
        if (std::memcmp(id, "fmt ", 4) == 0 && csz >= 16) {
            fmt = rd_u16(data + body + 0);
            channels = rd_u16(data + body + 2);
            sample_rate = (int)rd_u32(data + body + 4);
            bits = rd_u16(data + body + 14);
            // Support WAVE_FORMAT_EXTENSIBLE (0xFFFE)
            if (fmt == 0xFFFE && csz >= 40) {
                fmt = rd_u16(data + body + 24); // SubFormat GUID first 2 bytes
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_off = body;
            data_len = csz;
        }
        // chunks are word-aligned
        p = body + csz + (csz & 1);
    }

    if (channels <= 0 || sample_rate <= 0 || data_off == 0 || data_len == 0) return false;
    // Only PCM (1) and IEEE float (3) supported.
    if (fmt != 1 && fmt != 3) return false;

    out.sample_rate = sample_rate;
    out.channels = channels;
    out.bits = bits;
    out.pcm.clear();

    const uint8_t* s = data + data_off;
    size_t bytes_per_sample = (size_t)(bits / 8);
    if (bytes_per_sample == 0) return false;
    size_t nsamp = data_len / bytes_per_sample;
    if (channels > 0) nsamp -= nsamp % (size_t)channels;
    out.pcm.reserve(nsamp);

    if (fmt == 1 && bits == 16) {
        for (size_t i = 0; i < nsamp; ++i)
            out.pcm.push_back((int16_t)rd_u16(s + i * 2));
    } else if (fmt == 1 && bits == 8) {
        for (size_t i = 0; i < nsamp; ++i)
            out.pcm.push_back((int16_t)(((int)s[i] - 128) << 8));
    } else if (fmt == 1 && bits == 24) {
        for (size_t i = 0; i < nsamp; ++i) {
            const uint8_t* q = s + i * 3;
            int32_t v = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16));
            if (v & 0x800000) v |= (int32_t)0xFF000000;
            out.pcm.push_back((int16_t)(v >> 8));
        }
    } else if (fmt == 1 && bits == 32) {
        for (size_t i = 0; i < nsamp; ++i) {
            int32_t v = (int32_t)rd_u32(s + i * 4);
            out.pcm.push_back((int16_t)(v >> 16));
        }
    } else if (fmt == 3 && bits == 32) {
        for (size_t i = 0; i < nsamp; ++i) {
            float f;
            uint32_t u = rd_u32(s + i * 4);
            std::memcpy(&f, &u, 4);
            f = std::max(-1.0f, std::min(1.0f, f));
            out.pcm.push_back((int16_t)(f * 32767.0f));
        }
    } else if (fmt == 3 && bits == 64) {
        for (size_t i = 0; i < nsamp; ++i) {
            uint64_t u = 0;
            for (int b = 0; b < 8; ++b) u |= (uint64_t)s[i * 8 + b] << (8 * b);
            double d;
            std::memcpy(&d, &u, 8);
            d = std::max(-1.0, std::min(1.0, d));
            out.pcm.push_back((int16_t)(d * 32767.0));
        }
    } else {
        return false;
    }
    return true;
}

// Decode an Ogg Vorbis blob into 16-bit interleaved PCM.
bool decode_ogg(const uint8_t* data, size_t len, AudioClip& out) {
    if (len < 4 || std::memcmp(data, "OggS", 4) != 0) return false;
    int channels = 0, sample_rate = 0;
    short* pcm = nullptr;
    int samples = stb_vorbis_decode_memory(data, (int)len, &channels, &sample_rate, &pcm);
    if (samples <= 0 || !pcm || channels <= 0 || sample_rate <= 0) {
        if (pcm) free(pcm);
        return false;
    }
    out.sample_rate = sample_rate;
    out.channels = channels;
    out.bits = 16;
    out.pcm.assign(pcm, pcm + (size_t)samples * (size_t)channels);
    free(pcm);
    return true;
}

// Decode any supported container (WAV/RIFF or Ogg Vorbis), dispatching on magic.
bool decode_audio(const uint8_t* data, size_t len, AudioClip& out) {
    if (len >= 4 && std::memcmp(data, "RIFF", 4) == 0) return decode_wav(data, len, out);
    if (len >= 4 && std::memcmp(data, "OggS", 4) == 0) return decode_ogg(data, len, out);
    return false;
}

// =====================================================================
// Null backend (headless, deterministic)
// =====================================================================
class NullBackend : public AudioBackend {
public:
    bool open(int sr, int ch) override { m_sr = sr; m_ch = ch; m_frames = 0; return true; }
    void close() override {}
    void submit(const int16_t* samples, size_t count) override {
        (void)samples;
        if (m_ch > 0) m_frames += count / (size_t)m_ch;
    }
    uint64_t frames_played() const override { return m_frames; }
    bool real_device() const override { return false; }
private:
    int m_sr = 44100, m_ch = 2;
    uint64_t m_frames = 0;
};

std::unique_ptr<AudioBackend> make_null_backend() {
    return std::unique_ptr<AudioBackend>(new NullBackend());
}

// =====================================================================
// ALSA backend (host)
// =====================================================================
#ifdef GM14_HAVE_ALSA
class AlsaBackend : public AudioBackend {
public:
    ~AlsaBackend() override { close(); }
    bool open(int sr, int ch) override {
        snd_pcm_t* h = nullptr;
        if (snd_pcm_open(&h, m_dev.empty() ? "default" : m_dev.c_str(),
                         SND_PCM_STREAM_PLAYBACK, 0) < 0)
            return false;
        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca(&params);
        if (snd_pcm_hw_params_any(h, params) < 0 ||
            snd_pcm_hw_params_set_access(h, params, SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
            snd_pcm_hw_params_set_format(h, params, SND_PCM_FORMAT_S16_LE) < 0 ||
            snd_pcm_hw_params_set_channels(h, params, (unsigned)ch) < 0) {
            snd_pcm_close(h); return false;
        }
        unsigned rate = (unsigned)sr;
        snd_pcm_hw_params_set_rate_near(h, params, &rate, nullptr);
        snd_pcm_hw_params_set_period_size_near(h, params, (snd_pcm_uframes_t[]){1024}, nullptr);
        if (snd_pcm_hw_params(h, params) < 0) { snd_pcm_close(h); return false; }
        m_handle = h;
        m_sr = (int)rate;
        m_ch = ch;
        m_frames = 0;
        return true;
    }
    void close() override {
        if (m_handle) { snd_pcm_drain(m_handle); snd_pcm_close(m_handle); m_handle = nullptr; }
    }
    void submit(const int16_t* samples, size_t count) override {
        if (!m_handle) return;
        size_t frames = count / (size_t)m_ch;
        size_t done = 0;
        while (done < frames) {
            snd_pcm_sframes_t r = snd_pcm_writei(m_handle, samples + done * m_ch, frames - done);
            if (r < 0) {
                if (r == -EPIPE) { snd_pcm_prepare(m_handle); continue; }
                break;
            }
            done += (size_t)r;
        }
        m_frames += done;
    }
    uint64_t frames_played() const override { return m_frames; }
    bool real_device() const override { return true; }
    void set_device(const char* d) { m_dev = d ? d : ""; }
private:
    snd_pcm_t* m_handle = nullptr;
    std::string m_dev;
    int m_sr = 44100, m_ch = 2;
    uint64_t m_frames = 0;
};
#endif // GM14_HAVE_ALSA

// Forward declaration: defined after the platform backends below.
std::unique_ptr<AudioBackend> make_device_backend();

std::unique_ptr<AudioBackend> make_alsa_backend(const char* device) {
#ifdef GM14_HAVE_ALSA
    AlsaBackend* b = new AlsaBackend();
    if (device) b->set_device(device);
    return std::unique_ptr<AudioBackend>(b);
#else
    (void)device;
    return make_device_backend();
#endif
}

// =====================================================================
// Nintendo 3DS ndsp backend (Teak DSP).
//
// Renders stereo 16-bit PCM into two ping-pong sound buffers via
// linearAlloc(), and lets the DSP resample to its native rate with
// ndspChnSetRate(). The channel is configured once; submit() copies into the
// current buffer and waits for the prior wave to finish (simple blocking
// double-buffer, adequate for the frame-paced mixer).
// =====================================================================
#ifdef GM14_HAVE_NDSP
class NdspBackend : public AudioBackend {
public:
    ~NdspBackend() override { close(); }

    bool open(int sr, int ch) override {
        if (m_opened) return true;
        m_ch = (ch == 1) ? 1 : 2;
        // 4 x ~25ms buffers so the DSP always has queued work.
        m_buf_frames = (sr / 40) + 256;
        m_buf_bytes = (size_t)m_buf_frames * m_ch * sizeof(int16_t);
        for (int i = 0; i < kBufs; ++i) {
            m_buf[i] = (int16_t*)linearAlloc(m_buf_bytes);
            if (!m_buf[i]) { close(); return false; }
            std::memset(m_buf[i], 0, m_buf_bytes);
        }
        if (R_FAILED(ndspInit())) {
            for (int i = 0; i < kBufs; ++i) if (m_buf[i]) { linearFree(m_buf[i]); m_buf[i] = nullptr; }
            return false;
        }
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetCallback(&NdspBackend::dsp_callback, this);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
        ndspChnSetRate(0, (float)sr);
        ndspChnSetFormat(0, (m_ch == 1) ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
        m_cur = 0;
        m_fill = 0;
        m_frames = 0;
        m_opened = true;
        return true;
    }

    void close() override {
        if (m_opened) {
            ndspChnWaveBufClear(0);
            ndspExit();
            m_opened = false;
        }
        for (int i = 0; i < kBufs; ++i) if (m_buf[i]) { linearFree(m_buf[i]); m_buf[i] = nullptr; }
    }

    void submit(const int16_t* samples, size_t count) override {
        if (!m_opened) return;
        size_t samples_per_buf = (size_t)m_buf_frames * (size_t)m_ch;
        size_t i = 0;
        while (i < count) {
            size_t space = samples_per_buf - m_fill;
            size_t take = count - i < space ? count - i : space;
            std::memcpy(m_buf[m_cur] + m_fill, samples + i, take * sizeof(int16_t));
            m_fill += take;
            i += take;
            if (m_fill == samples_per_buf) flush_buffer();
        }
    }

    uint64_t frames_played() const override { return m_frames; }
    bool real_device() const override { return true; }

private:
    static constexpr int kBufs = 4;

    // libctru DSP done-callback: mark finished buffers free so they can be reused.
    static void dsp_callback(void* user) {
        NdspBackend* self = (NdspBackend*)user;
        for (int i = 0; i < kBufs; ++i) {
            if (self->m_wave[i].status == NDSP_WBUF_DONE) {
                self->m_wave[i].status = NDSP_WBUF_FREE;
            }
        }
    }

    void flush_buffer() {
        // Only submit when this slot is free; otherwise drop (mixer is ahead).
        if (m_wave[m_cur].status == NDSP_WBUF_FREE) {
            ndspWaveBuf& wb = m_wave[m_cur];
            wb.data_vaddr = m_buf[m_cur];
            wb.nsamples = (u32)m_buf_frames;
            wb.looping = false;
            DSP_FlushDataCache(m_buf[m_cur], m_buf_bytes);
            ndspChnWaveBufAdd(0, &wb);
            m_frames += (uint64_t)m_buf_frames;
        }
        m_cur = (m_cur + 1) % kBufs;
        m_fill = 0;
    }

    bool m_opened = false;
    int m_ch = 2;
    int m_buf_frames = 0;
    size_t m_buf_bytes = 0;
    int16_t* m_buf[kBufs] = { nullptr, nullptr, nullptr, nullptr };
    ndspWaveBuf m_wave[kBufs] = {};
    int m_cur = 0;
    size_t m_fill = 0;
    uint64_t m_frames = 0;
};
#endif // GM14_HAVE_NDSP

#ifdef GM14_HAVE_WAVEOUT
class WaveOutBackend : public AudioBackend {
public:
    ~WaveOutBackend() override { close(); }

    bool open(int sr, int ch) override {
        if (m_opened) return true;
        if (waveOutGetNumDevs() == 0) return false;

        m_sr = sr;
        m_ch = (ch == 1) ? 1 : 2;

        WAVEFORMATEX wfx;
        std::memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD)m_ch;
        wfx.nSamplesPerSec = (DWORD)m_sr;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = (WORD)(m_ch * sizeof(int16_t));
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize = 0;

        m_hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_hEvent) return false;

        MMRESULT res = waveOutOpen(&m_hWaveOut, WAVE_MAPPER, &wfx,
                                   (DWORD_PTR)m_hEvent, 0, CALLBACK_EVENT);
        if (res != MMSYSERR_NOERROR) {
            CloseHandle(m_hEvent);
            m_hEvent = nullptr;
            m_hWaveOut = nullptr;
            return false;
        }

        m_buf_frames = (sr / 40) + 256;
        m_buf_bytes = (size_t)m_buf_frames * (size_t)m_ch * sizeof(int16_t);

        for (int i = 0; i < kBufs; ++i) {
            m_buf[i].assign((size_t)m_buf_frames * (size_t)m_ch, 0);
            std::memset(&m_wave[i], 0, sizeof(WAVEHDR));
            m_in_flight[i] = false;
        }

        m_cur = 0;
        m_fill = 0;
        m_frames = 0;
        m_opened = true;
        return true;
    }

    void close() override {
        if (m_opened) {
            if (m_hWaveOut) {
                waveOutReset(m_hWaveOut);
                for (int i = 0; i < kBufs; ++i) {
                    if (m_in_flight[i]) {
                        waveOutUnprepareHeader(m_hWaveOut, &m_wave[i], sizeof(WAVEHDR));
                        m_in_flight[i] = false;
                    }
                }
                waveOutClose(m_hWaveOut);
                m_hWaveOut = nullptr;
            }
            if (m_hEvent) {
                CloseHandle(m_hEvent);
                m_hEvent = nullptr;
            }
            m_opened = false;
        }
    }

    void submit(const int16_t* samples, size_t count) override {
        if (!m_opened || !m_hWaveOut) return;
        size_t samples_per_buf = (size_t)m_buf_frames * (size_t)m_ch;
        size_t i = 0;
        while (i < count) {
            size_t space = samples_per_buf - m_fill;
            size_t take = (count - i < space) ? (count - i) : space;
            std::memcpy(m_buf[m_cur].data() + m_fill, samples + i, take * sizeof(int16_t));
            m_fill += take;
            i += take;
            if (m_fill == samples_per_buf) flush_buffer();
        }
    }

    uint64_t frames_played() const override { return m_frames; }
    bool real_device() const override { return true; }

private:
    static constexpr int kBufs = 4;

    void flush_buffer() {
        if (!m_hWaveOut) return;

        if (m_in_flight[m_cur]) {
            while (m_in_flight[m_cur] && !(m_wave[m_cur].dwFlags & WHDR_DONE)) {
                if (m_hEvent) {
                    DWORD wr = WaitForSingleObject(m_hEvent, 50);
                    if (wr == WAIT_TIMEOUT && !(m_wave[m_cur].dwFlags & WHDR_DONE)) {
                        break;
                    }
                } else {
                    Sleep(1);
                }
            }
            if (m_wave[m_cur].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(m_hWaveOut, &m_wave[m_cur], sizeof(WAVEHDR));
                m_in_flight[m_cur] = false;
            } else {
                m_cur = (m_cur + 1) % kBufs;
                m_fill = 0;
                return;
            }
        }

        m_wave[m_cur].lpData = (LPSTR)m_buf[m_cur].data();
        m_wave[m_cur].dwBufferLength = (DWORD)m_buf_bytes;
        m_wave[m_cur].dwFlags = 0;

        if (waveOutPrepareHeader(m_hWaveOut, &m_wave[m_cur], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            if (waveOutWrite(m_hWaveOut, &m_wave[m_cur], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                m_in_flight[m_cur] = true;
                m_frames += (uint64_t)m_buf_frames;
            } else {
                waveOutUnprepareHeader(m_hWaveOut, &m_wave[m_cur], sizeof(WAVEHDR));
            }
        }

        m_cur = (m_cur + 1) % kBufs;
        m_fill = 0;
    }

    HWAVEOUT m_hWaveOut = nullptr;
    HANDLE m_hEvent = nullptr;
    bool m_opened = false;
    int m_sr = 44100;
    int m_ch = 2;
    int m_buf_frames = 0;
    size_t m_buf_bytes = 0;
    std::vector<int16_t> m_buf[kBufs];
    WAVEHDR m_wave[kBufs] = {};
    bool m_in_flight[kBufs] = {false};
    int m_cur = 0;
    size_t m_fill = 0;
    uint64_t m_frames = 0;
};
#endif // GM14_HAVE_WAVEOUT

// Pick the best real-device backend for the current platform.
std::unique_ptr<AudioBackend> make_device_backend() {
#if defined(GM14_HAVE_NDSP)
    return std::unique_ptr<AudioBackend>(new NdspBackend());
#elif defined(GM14_HAVE_WAVEOUT)
    return std::unique_ptr<AudioBackend>(new WaveOutBackend());
#elif defined(GM14_HAVE_ALSA)
    return std::unique_ptr<AudioBackend>(new AlsaBackend());
#else
    return make_null_backend();
#endif
}

std::unique_ptr<AudioBackend> make_waveout_backend() {
#if defined(GM14_HAVE_WAVEOUT)
    return std::unique_ptr<AudioBackend>(new WaveOutBackend());
#else
    return make_device_backend();
#endif
}

// =====================================================================
// Mixer
// =====================================================================
AudioEngine::AudioEngine() {
    m_backend = make_null_backend();
    m_backend->open(m_sample_rate, m_channels);
    m_voices.resize(m_max_voices);
}

AudioEngine::~AudioEngine() {
    if (m_backend) m_backend->close();
}

void AudioEngine::set_backend(std::unique_ptr<AudioBackend> b) {
    if (m_backend) m_backend->close();
    m_backend = std::move(b);
    // If the requested device backend fails to open (e.g. ndsp service
    // unavailable), fall back to the null backend so the game keeps running.
    if (m_backend && !m_backend->open(m_sample_rate, m_channels)) {
        m_backend = make_null_backend();
        m_backend->open(m_sample_rate, m_channels);
    }
}

void AudioEngine::set_clip(int asset, std::shared_ptr<AudioClip> clip) {
    if (asset < 0) return;
    if ((int)m_clips.size() <= asset) m_clips.resize(asset + 1);
    if ((int)m_clip_used.size() <= asset) m_clip_used.resize(asset + 1);
    // account bytes for the budget (replace any previous clip)
    if (m_clips[asset]) {
        size_t old_bytes = m_clips[asset]->pcm.size() * sizeof(int16_t);
        m_resident_bytes = (old_bytes >= m_resident_bytes) ? 0 : (m_resident_bytes - old_bytes);
    }
    if (clip) {
        m_resident_bytes += clip->pcm.size() * sizeof(int16_t);
    }
    m_clips[asset] = std::move(clip);
    m_clip_used[asset] = ++m_use_counter;
    evict_if_needed(asset);
}

bool AudioEngine::has_clip(int asset) const {
    return asset >= 0 && asset < (int)m_clips.size() && m_clips[asset] != nullptr;
}

void AudioEngine::set_clip_loader(ClipLoader loader, size_t budget_bytes) {
    m_loader = std::move(loader);
    m_budget_bytes = budget_bytes;
    evict_if_needed();
}

void AudioEngine::evict_if_needed(int protect_asset) {
    if (m_budget_bytes == 0) return;
    while (m_resident_bytes > m_budget_bytes) {
        // Find the least-recently-used clip that is not currently playing and not protected.
        int victim = -1;
        uint64_t oldest = ~0ull;
        for (size_t a = 0; a < m_clips.size(); ++a) {
            if (!m_clips[a]) continue;
            if ((int)a == protect_asset) continue;
            bool playing = false;
            for (auto& v : m_voices) {
                if (v.active && v.asset == (int)a) {
                    playing = true;
                    break;
                }
            }
            if (playing) continue;
            if (m_clip_used[a] < oldest) {
                oldest = m_clip_used[a];
                victim = (int)a;
            }
        }
        if (victim < 0) break; // everything resident is playing or protected; give up
        size_t clip_bytes = m_clips[victim]->pcm.size() * sizeof(int16_t);
        m_resident_bytes = (clip_bytes >= m_resident_bytes) ? 0 : (m_resident_bytes - clip_bytes);
        m_clips[victim].reset();
    }
}

const AudioClip* AudioEngine::acquire(int asset) {
    if (asset < 0) return nullptr;
    if (!has_clip(asset) && m_loader) {
        std::shared_ptr<AudioClip> c = m_loader(asset);
        if (c) {
            set_clip(asset, c);
        }
    }
    if (!has_clip(asset)) return nullptr;
    if ((int)m_clip_used.size() <= asset) m_clip_used.resize(asset + 1);
    m_clip_used[asset] = ++m_use_counter;
    return m_clips[asset].get();
}

Voice* AudioEngine::find_free_voice() {
    for (auto& v : m_voices) if (!v.active) return &v;
    // steal the lowest-priority (then lowest-gain) voice
    Voice* worst = nullptr;
    for (auto& v : m_voices) {
        if (!worst || v.priority < worst->priority) worst = &v;
    }
    return worst;
}

Voice* AudioEngine::voice_for_handle(int handle) {
    for (auto& v : m_voices) if (v.active && v.handle == handle) return &v;
    return nullptr;
}

int AudioEngine::play(int asset, bool loop, double gain, double offset_seconds, double pitch, double priority) {
    if (asset < 0) return -1;
    const AudioClip* clip = acquire(asset);
    if (!clip) return -1;
    Voice* v = find_free_voice();
    if (!v) return -1;
    v->active = true;
    v->paused = false;
    v->loop = loop;
    v->asset = asset;
    v->handle = m_next_handle++;
    v->gain = gain;
    v->pitch = pitch > 0 ? pitch : 1.0;
    v->priority = priority;
    const AudioClip& c = *clip;
    double start_frame = offset_seconds * c.sample_rate;
    if (start_frame < 0) start_frame = 0;
    if (start_frame >= (double)(c.pcm.size() / (size_t)c.channels)) start_frame = 0;
    v->cursor = start_frame;
    v->position = start_frame;
    return v->handle;
}

void AudioEngine::stop_asset(int asset) {
    for (auto& v : m_voices) if (v.active && v.asset == asset) v.active = false;
}
void AudioEngine::stop_handle(int handle) {
    Voice* v = voice_for_handle(handle);
    if (v) v->active = false;
}
void AudioEngine::stop_all() { for (auto& v : m_voices) v.active = false; }

void AudioEngine::pause_asset(int asset) {
    for (auto& v : m_voices) if (v.active && v.asset == asset) v.paused = true;
}
void AudioEngine::pause_handle(int handle) {
    Voice* v = voice_for_handle(handle); if (v) v->paused = true;
}
void AudioEngine::resume_asset(int asset) {
    for (auto& v : m_voices) if (v.active && v.asset == asset) v.paused = false;
}
void AudioEngine::resume_handle(int handle) {
    Voice* v = voice_for_handle(handle); if (v) v->paused = false;
}
void AudioEngine::pause_all() { for (auto& v : m_voices) if (v.active) v.paused = true; }
void AudioEngine::resume_all() { for (auto& v : m_voices) if (v.active) v.paused = false; }

void AudioEngine::set_gain_asset(int asset, double g) {
    for (auto& v : m_voices) if (v.active && v.asset == asset) v.gain = g;
}
void AudioEngine::set_gain_handle(int handle, double g) {
    Voice* v = voice_for_handle(handle); if (v) v->gain = g;
}
double AudioEngine::get_gain_asset(int asset) const {
    for (const auto& v : m_voices) if (v.active && v.asset == asset) return v.gain;
    return 0.0;
}
double AudioEngine::get_gain_handle(int handle) const {
    for (const auto& v : m_voices) if (v.active && v.handle == handle) return v.gain;
    return 0.0;
}

void AudioEngine::set_pitch_asset(int asset, double p) {
    for (auto& v : m_voices) if (v.active && v.asset == asset) v.pitch = (p > 0 ? p : 1.0);
}
void AudioEngine::set_pitch_handle(int handle, double p) {
    Voice* v = voice_for_handle(handle); if (v) v->pitch = (p > 0 ? p : 1.0);
}
double AudioEngine::get_pitch_asset(int asset) const {
    for (const auto& v : m_voices) if (v.active && v.asset == asset) return v.pitch;
    return 1.0;
}
double AudioEngine::get_pitch_handle(int handle) const {
    for (const auto& v : m_voices) if (v.active && v.handle == handle) return v.pitch;
    return 1.0;
}

double AudioEngine::get_track_position(int id) const {
    if (id >= kHandleBase) {
        for (const auto& v : m_voices)
            if (v.active && v.handle == id && has_clip(v.asset)) {
                int sr = m_clips[v.asset]->sample_rate;
                return (sr > 0) ? (v.cursor / (double)sr) : 0.0;
            }
        return 0.0;
    }
    for (const auto& v : m_voices)
        if (v.active && v.asset == id && has_clip(v.asset)) {
            int sr = m_clips[v.asset]->sample_rate;
            return (sr > 0) ? (v.cursor / (double)sr) : 0.0;
        }
    return 0.0;
}
void AudioEngine::set_track_position(int handle, double seconds) {
    Voice* v = voice_for_handle(handle);
    if (!v || !has_clip(v->asset)) return;
    int sr = m_clips[v->asset]->sample_rate;
    if (sr <= 0) return;
    double frame = seconds * sr;
    if (frame < 0) frame = 0;
    v->cursor = frame;
}

bool AudioEngine::is_playing(int id) const {
    if (id >= kHandleBase) {
        for (const auto& v : m_voices)
            if (v.active && v.handle == id && !v.paused) return true;
        return false;
    }
    for (const auto& v : m_voices)
        if (v.active && v.asset == id && !v.paused) return true;
    return false;
}
bool AudioEngine::is_paused(int id) const {
    if (id >= kHandleBase) {
        for (const auto& v : m_voices)
            if (v.active && v.handle == id && v.paused) return true;
        return false;
    }
    for (const auto& v : m_voices)
        if (v.active && v.asset == id && v.paused) return true;
    return false;
}

void AudioEngine::set_master_gain(double g) { m_master_gain = g; }
void AudioEngine::set_max_voices(int n) {
    if (n < 1) n = 1;
    m_max_voices = n;
    m_voices.resize(n);
}

void AudioEngine::mix_voice(Voice& v, int frames) {
    if (v.paused) return;
    const AudioClip& c = *m_clips[v.asset];
    int vc = c.channels;
    if (vc <= 0) { v.active = false; return; }
    size_t total_frames = c.pcm.size() / (size_t)vc;
    if (total_frames == 0) { v.active = false; return; }

    // Advance the source cursor by pitch * (clip_rate / output_rate) so clips
    // recorded at a different rate still play at the correct speed.
    double rate_ratio = (c.sample_rate > 0) ? ((double)c.sample_rate / (double)m_sample_rate) : 1.0;
    double step = v.pitch * rate_ratio;
    double gain = v.gain * m_master_gain;

    for (int f = 0; f < frames; ++f) {
        if (!v.active) break;
        if (v.cursor >= (double)total_frames) {
            if (v.loop) {
                v.cursor = std::fmod(v.cursor, (double)total_frames);
            } else {
                v.active = false;
                break;
            }
        }
        if (v.cursor < 0) v.cursor = 0;
        size_t i0 = (size_t)v.cursor;
        size_t i1 = i0 + 1;
        if (i1 >= total_frames) i1 = v.loop ? 0 : i0;
        double frac = v.cursor - (double)i0;
        for (int ch = 0; ch < m_channels; ++ch) {
            int sc = (ch < vc) ? ch : (vc - 1);
            double a = c.pcm[i0 * vc + sc];
            double b = c.pcm[i1 * vc + sc];
            double s = (a + (b - a) * frac) * gain;
            int idx = f * m_channels + ch;
            int32_t mixed = m_mix[idx] + (int32_t)std::lround(s);
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            m_mix[idx] = (int16_t)mixed;
        }
        v.cursor += step;
        v.position = v.cursor;
    }
}

void AudioEngine::render(int frames) {
    if (frames <= 0) return;
    m_mix.assign((size_t)frames * m_channels, 0);
    for (auto& v : m_voices) {
        if (v.active && has_clip(v.asset)) mix_voice(v, frames);
    }
    m_backend->submit(m_mix.data(), m_mix.size());
    m_mix.clear();
}

void AudioEngine::flush() {}

} // namespace gm14
