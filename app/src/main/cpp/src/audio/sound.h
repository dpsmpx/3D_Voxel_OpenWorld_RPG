/**
 * @file sound.h
 * @brief Звук: движок AAudio, процедурные эффекты, динамическая музыка.
 */
#pragma once
#include "../core/types.h"
#include <vector>

namespace audio {

/// Звуковой буфер. Моно, PCM float32.
/// Длительность вычисляется как frames / sampleRate.
struct Sound {
    std::vector<f32> samples;   // размер = frames
    u32 sampleRate = 48000;
    u32 frames     = 0;
    bool looping   = false;
    f32 defaultGain = 1.0f;

    bool valid() const {
        return frames > 0 && samples.size() == frames;
    }

    f32 duration() const {
        return sampleRate > 0 ? (f32)frames / (f32)sampleRate : 0.f;
    }

    /// Сэмпл в произвольной позиции (без интерполяции).
    inline f32 at(u32 frame) const {
        if (frame >= frames) return 0.f;
        return samples[frame];
    }
};

} // namespace audio
