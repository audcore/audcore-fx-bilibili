#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace bilibili {
inline constexpr unsigned defaultVolume = 50, volumeParameter = 1;
inline unsigned volumeFromNormalized(double value) {
    return static_cast<unsigned>(std::lround(std::clamp(value, 0., 1.) * 100.));
}
inline const auto volumeGains = [] {
    // Half scale maps to -10 dB. Calculate the curve before starting the audio callback.
    std::array<float, 101> gains{};
    for (unsigned level = 1; level < gains.size(); ++level)
        gains[level] = static_cast<float>(std::pow(level / 100., 1.66096404744));
    return gains;
}();

struct Selection {
    std::string id;
    uint16_t part = 1;

    bool operator==(const Selection& other) const { return id == other.id && part == other.part; }
};
class Audio {
    struct Frame { float left, right; uint32_t sourceRate; };
    static constexpr uint32_t capacity = 16384, mask = capacity - 1;
    std::array<Frame, capacity> queue{};
    std::atomic<uint32_t> head{0}, tail{0}, flush{0};
    uint32_t appliedFlush = 0;
    double fraction = 0;
    uint32_t lastSourceRate = 0;
    bool primed = false;
    float gain = volumeGains[defaultVolume];
    uint32_t channels = 2;
    mutable std::mutex selectionMutex;
    Selection selection;
    std::map<const void*, std::function<void()>> selectionListeners;

public:
    std::atomic<uint32_t> rate{0};
    std::atomic<uint32_t> volume{defaultVolume};
    std::atomic<bool> bypass{false}, playing{false};

    Selection video() const { std::lock_guard<std::mutex> lock(selectionMutex); return selection; }
    Selection watchSelection(const void* owner, std::function<void()> notify) {
        std::lock_guard<std::mutex> lock(selectionMutex);
        selectionListeners.insert_or_assign(owner, std::move(notify));
        return selection;
    }
    void unwatchSelection(const void* owner) {
        std::lock_guard<std::mutex> lock(selectionMutex);
        selectionListeners.erase(owner);
    }
    // Only controller and editor threads change the selection. Listeners enqueue a message.
    bool select(Selection value, const void* source = nullptr, bool notifyUnchanged = false) {
        std::lock_guard<std::mutex> lock(selectionMutex);
        const bool changed = !(selection == value);
        if (changed) selection = std::move(value);
        if (changed || notifyUnchanged)
            for (const auto& [owner, notify] : selectionListeners)
                if (owner != source) notify();
        return changed;
    }

    void reset() noexcept { flush.fetch_add(1, std::memory_order_release); }

    void prepare(uint32_t sampleRate, uint32_t channelCount) noexcept {
        channels = channelCount;
        reset();
        rate.store(sampleRate, std::memory_order_release);
    }

    bool write(const float* samples, uint32_t frames, uint32_t sourceChannels, uint32_t sourceRate) noexcept {
        if (!playing.load(std::memory_order_acquire) || bypass.load(std::memory_order_relaxed)) return true;
        const auto first = head.load(std::memory_order_relaxed);
        if (frames > capacity - (first - tail.load(std::memory_order_acquire))) return false;
        for (uint32_t index = 0; index < frames; ++index)
            queue[(first + index) & mask] = {samples[sourceChannels * index],
                samples[sourceChannels * index + sourceChannels - 1], sourceRate};
        head.store(first + frames, std::memory_order_release);
        return true;
    }

    void mix(float* stereo, uint32_t frames) noexcept {
        const auto end = head.load(std::memory_order_acquire);
        const auto generation = flush.load(std::memory_order_acquire);
        auto cursor = tail.load(std::memory_order_relaxed);
        if (appliedFlush != generation || !playing.load(std::memory_order_acquire) || bypass.load(std::memory_order_relaxed)) {
            appliedFlush = generation;
            tail.store(end, std::memory_order_release);
            fraction = 0;
            lastSourceRate = 0;
            primed = false;
            return;
        }
        const auto sampleRate = rate.load(std::memory_order_relaxed);
        if (!sampleRate || !stereo) return;
        if (end == cursor) { primed = false; return; }
        const auto sourceRate = queue[cursor & mask].sourceRate;
        if (!primed && end - cursor < sourceRate * 64 / 1000) return;
        const float targetGain = volumeGains[volume.load(std::memory_order_relaxed)];
        if (!primed) gain = targetGain;
        primed = true;
        const float gainStep = 100.f / static_cast<float>(sampleRate);
        uint32_t blockSourceRate = 0;
        double ratio = 0;
        for (uint32_t index = 0; index < frames; ++index) {
            const auto& a = queue[cursor & mask];
            if (a.sourceRate != blockSourceRate) {
                blockSourceRate = a.sourceRate;
                const auto target = blockSourceRate * 64 / 1000;
                const auto correction = std::clamp((static_cast<double>(end - cursor) - target) * 0.000004, -0.01, 0.01);
                ratio = static_cast<double>(blockSourceRate) / sampleRate * (1.0 + correction);
                if (lastSourceRate != blockSourceRate) { fraction = 0; lastSourceRate = blockSourceRate; }
            }
            const double next = fraction + ratio;
            const auto advance = static_cast<uint32_t>(next);
            if (end - cursor <= std::max(advance, 1u)) {
                primed = false;
                break;
            }
            const auto& nextFrame = queue[(cursor + 1) & mask];
            const auto& b = nextFrame.sourceRate == a.sourceRate ? nextFrame : a;
            gain += std::clamp(targetGain - gain, -gainStep, gainStep);
            const float left = (a.left + static_cast<float>(fraction) * (b.left - a.left)) * gain;
            const float right = (a.right + static_cast<float>(fraction) * (b.right - a.right)) * gain;
            if (channels == 1) {
                stereo[2 * index] += (left + right) * .5f;
                stereo[2 * index + 1] = stereo[2 * index];
            } else {
                stereo[2 * index] += left;
                stereo[2 * index + 1] += right;
            }
            cursor += advance;
            fraction = next - advance;
        }
        tail.store(cursor, std::memory_order_release);
    }
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
std::shared_ptr<Audio> audioFor(uint32_t id);
}
