#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <cassert>
#include <deque>
#include <mutex>

static uint32_t sample_rate = 48000;

static ultramodern::audio_callbacks_t audio_callbacks;

// Buffers handed over by the game but not played yet (the AI holds at most two; a few more
// are allowed in case the game runs ahead). Each keeps the sample rate that was set when it
// was handed over, so a rate change applies from the right buffer on.
struct PendingAudioBuffer {
    PTR(int16_t) data;
    uint32_t sample_count;
    uint32_t frequency;
};
static std::mutex audio_mutex;
static std::deque<PendingAudioBuffer> pending_audio_buffers;
static uint8_t* audio_rdram = nullptr;
static uint32_t applied_frequency = 0;
static std::atomic<uint32_t> last_audio_buffer_frames{0};

uint32_t ultramodern::get_last_audio_buffer_frames() {
    return last_audio_buffer_frames.load();
}

uint32_t ultramodern::get_queued_audio_frames() {
    return (audio_callbacks.get_frames_remaining != nullptr) ? uint32_t(audio_callbacks.get_frames_remaining()) : 0;
}

void ultramodern::set_audio_callbacks(const ultramodern::audio_callbacks_t& callbacks) {
    audio_callbacks = callbacks;
}

void ultramodern::init_audio() {
    // Pick an initial dummy sample rate; this will be set by the game later to the true sample rate.
    set_audio_frequency(48000);
}

static void apply_frequency(uint32_t freq) {
    if (freq != applied_frequency) {
        if (audio_callbacks.set_frequency) {
            audio_callbacks.set_frequency(freq);
        }
        applied_frequency = freq;
    }
}

void ultramodern::set_audio_frequency(uint32_t freq) {
    std::lock_guard lock{ audio_mutex };
    sample_rate = freq;
    // With nothing waiting to play, the output can switch right away.
    if (pending_audio_buffers.empty()) {
        apply_frequency(freq);
    }
}

void ultramodern::queue_audio_buffer(RDRAM_ARG PTR(int16_t) audio_data_, uint32_t byte_count) {
    // Ensure that the byte count is an integer multiple of samples.
    assert((byte_count & 1) == 0);

    // Calculate the number of samples from the number of bytes.
    uint32_t sample_count = byte_count / sizeof(int16_t);

    std::lock_guard lock{ audio_mutex };
    audio_rdram = rdram;
    if (pending_audio_buffers.size() >= 8) {
        pending_audio_buffers.pop_front(); // the game ran far ahead; drop the oldest
    }
    pending_audio_buffers.push_back({ audio_data_, sample_count, sample_rate });
    last_audio_buffer_frames = sample_count / 2;
}

bool ultramodern::play_pending_audio_buffer() {
    std::lock_guard lock{ audio_mutex };
    if (pending_audio_buffers.empty()) {
        return false;
    }
    PendingAudioBuffer buffer = pending_audio_buffers.front();
    pending_audio_buffers.pop_front();
    apply_frequency(buffer.frequency);
    uint8_t* rdram = audio_rdram;
    if (buffer.sample_count > 0 && audio_callbacks.queue_samples) {
        audio_callbacks.queue_samples(TO_PTR(int16_t, buffer.data), buffer.sample_count);
    }
    return true;
}

// For SDL2
//uint32_t buffer_offset_frames = 1;
// For Godot
float buffer_offset_frames = 0.5f;

// If there's ever any audio popping, check here first. Some games are very sensitive to
// the remaining sample count and reporting a number that's too high here can lead to issues.
// Reporting a number that's too low can lead to audio lag in some games.
uint32_t ultramodern::get_remaining_audio_bytes() {
    // Get the number of remaining buffered audio bytes.
    uint32_t buffered_byte_count;
    if (audio_callbacks.get_frames_remaining != nullptr) {
        buffered_byte_count = audio_callbacks.get_frames_remaining() * 2 * sizeof(int16_t);
    }
    else {
        buffered_byte_count = 100;
    }
    // Adjust the reported count to be some number of refreshes in the future, which helps ensure that
    // there are enough samples even if the audio thread experiences a small amount of lag. This prevents
    // audio popping on games that use the buffered audio byte count to determine how many samples
    // to generate.
    uint32_t samples_per_vi = (sample_rate / 60);
    if (buffered_byte_count > static_cast<uint32_t>(buffer_offset_frames * sizeof(int16_t) * samples_per_vi)) {
        buffered_byte_count -= static_cast<uint32_t>(buffer_offset_frames * sizeof(int16_t) * samples_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    return buffered_byte_count;
}
