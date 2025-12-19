#include "stdafx.h"

#include "libretro_audio.h"
#include "libretro_core.h"

#include <algorithm>
#include <cstring>
#include <chrono>

LOG_CHANNEL(libretro_audio_log, "LibretroAudio");

static LibretroAudioBackend* s_audio_backend = nullptr;

LibretroAudioBackend* get_libretro_audio_backend()
{
    return s_audio_backend;
}

void libretro_audio_process(retro_audio_sample_batch_t audio_batch_cb)
{
    if (!audio_batch_cb || !s_audio_backend)
        return;

    // Per libretro docs: push ~1/fps seconds of audio per retro_run()
    // At 48kHz/60fps = 800 frames per video frame
    // Push in smaller chunks to reduce latency and stutter
    // Process multiple batches to drain available audio
    static constexpr size_t FRAMES_PER_BATCH = 512;
    static constexpr int MAX_BATCHES = 4;  // Up to ~2048 frames per retro_run
    alignas(16) int16_t buffer[FRAMES_PER_BATCH * 2]; // 16-byte aligned for SIMD

    for (int batch = 0; batch < MAX_BATCHES; batch++)
    {
        size_t frames = s_audio_backend->GetSamples(buffer, FRAMES_PER_BATCH);
        if (frames > 0)
        {
            audio_batch_cb(buffer, frames);
        }
        if (frames < FRAMES_PER_BATCH)
        {
            // No more data available
            break;
        }
    }
}

LibretroAudioBackend::LibretroAudioBackend()
{
    s_audio_backend = this;
    libretro_audio_log.notice("LibretroAudioBackend created");
}

LibretroAudioBackend::~LibretroAudioBackend()
{
    Close();
    if (s_audio_backend == this)
        s_audio_backend = nullptr;
    libretro_audio_log.notice("LibretroAudioBackend destroyed");
}

bool LibretroAudioBackend::Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout)
{
    (void)dev_id;

    std::lock_guard lock(m_mutex);

    // Set base class members - use what the config passes us
    // DO NOT force S16 here - let the config control this via raw.convert_to_s16
    // We handle float->s16 conversion ourselves in GetSamples() if needed
    m_sampling_rate = freq;
    m_sample_size = sample_size;  // Accept what config gives us (usually FLOAT)
    m_channels = static_cast<u32>(ch_cnt);
    m_layout = layout;

    // Ring buffer for ~500ms of audio - larger buffer reduces stutter
    // At 48kHz stereo float = 48000 * 2 * 4 * 0.5 = 192KB
    const size_t bytes_per_second = static_cast<size_t>(freq) * static_cast<u32>(ch_cnt) * get_sample_size();
    m_ring_buffer_bytes.resize(bytes_per_second / 2, 0);  // 500ms buffer
    m_ring_read_pos = 0;
    m_ring_write_pos = 0;
    m_ring_size = 0;

    m_initialized = true;
    libretro_audio_log.notice("LibretroAudioBackend::Open() freq=%u ch=%u sample_size=%u ring_buffer_bytes=%zu convert_to_s16=%d",
        static_cast<u32>(freq), static_cast<u32>(ch_cnt), get_sample_size(), m_ring_buffer_bytes.size(), get_convert_to_s16() ? 1 : 0);
    return true;
}

void LibretroAudioBackend::Close()
{
    std::lock_guard lock(m_mutex);

    m_playing = false;
    m_initialized = false;
    m_ring_buffer_bytes.clear();
    m_ring_read_pos = 0;
    m_ring_write_pos = 0;
    m_ring_size = 0;
    libretro_audio_log.notice("LibretroAudioBackend::Close()");
}

void LibretroAudioBackend::SetWriteCallback(std::function<u32(u32, void*)> cb)
{
    std::lock_guard lock(m_mutex);
    m_write_callback = std::move(cb);
}

void LibretroAudioBackend::SetStateCallback(std::function<void(AudioStateEvent)> cb)
{
    std::lock_guard lock(m_mutex);
    m_state_callback = std::move(cb);
}

f64 LibretroAudioBackend::GetCallbackFrameLen()
{
    // Return frame length in seconds
    // For libretro, we want callbacks frequently to keep the ring buffer filled
    // 256 samples at 48kHz = ~5.3ms per callback
    return 256.0 / static_cast<f64>(get_sampling_rate());
}

void LibretroAudioBackend::Play()
{
    m_playing = true;
}

void LibretroAudioBackend::Pause()
{
    m_playing = false;
}

size_t LibretroAudioBackend::GetSamples(int16_t* buffer, size_t max_frames)
{
    if (!m_playing || !m_initialized)
        return 0;

    // Use timed lock to avoid skipping frames but also avoid deadlock
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::microseconds(100)))
        return 0;

    const u32 sample_size = get_sample_size();  // 4 for float, 2 for s16
    const u32 channels = get_channels();
    const bool is_float = (m_sample_size == AudioSampleSize::FLOAT);
    const size_t bytes_per_frame = channels * sample_size;

    // Aggressively fill our ring buffer from the emulator's audio callback
    // Pull multiple times to ensure buffer stays full
    if (m_write_callback)
    {
        static constexpr size_t PULL_FRAMES = 2048;
        const size_t pull_bytes = PULL_FRAMES * bytes_per_frame;
        alignas(16) u8 temp_buffer[PULL_FRAMES * 2 * sizeof(float)];

        // Pull until buffer is reasonably full or no more data
        for (int pulls = 0; pulls < 4; pulls++)
        {
            const size_t free_space = m_ring_buffer_bytes.size() - m_ring_size;
            if (free_space < pull_bytes)
                break;

            const u32 bytes_written = m_write_callback(static_cast<u32>(pull_bytes), temp_buffer);
            if (bytes_written == 0)
                break;

            // Push to ring buffer - use memcpy for efficiency when possible
            if (m_ring_write_pos + bytes_written <= m_ring_buffer_bytes.size())
            {
                // Contiguous write
                std::memcpy(&m_ring_buffer_bytes[m_ring_write_pos], temp_buffer, bytes_written);
                m_ring_write_pos += bytes_written;
                if (m_ring_write_pos >= m_ring_buffer_bytes.size())
                    m_ring_write_pos = 0;
            }
            else
            {
                // Wrap-around write
                const size_t first_part = m_ring_buffer_bytes.size() - m_ring_write_pos;
                std::memcpy(&m_ring_buffer_bytes[m_ring_write_pos], temp_buffer, first_part);
                std::memcpy(&m_ring_buffer_bytes[0], temp_buffer + first_part, bytes_written - first_part);
                m_ring_write_pos = bytes_written - first_part;
            }
            m_ring_size += bytes_written;
        }
    }

    // Calculate how many frames we can provide
    const size_t frames_available = std::min(max_frames, m_ring_size / bytes_per_frame);

    if (frames_available == 0)
        return 0;

    const size_t bytes_to_read = frames_available * bytes_per_frame;

    // Read from ring buffer into a contiguous temp buffer for easier processing
    alignas(16) u8 read_buffer[2048 * 2 * sizeof(float)];
    if (m_ring_read_pos + bytes_to_read <= m_ring_buffer_bytes.size())
    {
        // Contiguous read
        std::memcpy(read_buffer, &m_ring_buffer_bytes[m_ring_read_pos], bytes_to_read);
        m_ring_read_pos += bytes_to_read;
        if (m_ring_read_pos >= m_ring_buffer_bytes.size())
            m_ring_read_pos = 0;
    }
    else
    {
        // Wrap-around read
        const size_t first_part = m_ring_buffer_bytes.size() - m_ring_read_pos;
        std::memcpy(read_buffer, &m_ring_buffer_bytes[m_ring_read_pos], first_part);
        std::memcpy(read_buffer + first_part, &m_ring_buffer_bytes[0], bytes_to_read - first_part);
        m_ring_read_pos = bytes_to_read - first_part;
    }
    m_ring_size -= bytes_to_read;

    // Convert to s16 for libretro
    if (is_float)
    {
        const float* src = reinterpret_cast<const float*>(read_buffer);
        const size_t total_samples = frames_available * channels;
        for (size_t i = 0; i < total_samples; i++)
        {
            float sample = std::clamp(src[i], -1.0f, 1.0f);
            buffer[i] = static_cast<int16_t>(sample * 32767.0f);
        }
    }
    else
    {
        // Already s16, just copy
        std::memcpy(buffer, read_buffer, bytes_to_read);
    }

    return frames_available;
}
