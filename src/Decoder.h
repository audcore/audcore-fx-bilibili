#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bilibili {
class Audio;

struct VideoArea {
    uint32_t x, y, width, height;
};

struct VideoFrame {
    uint32_t width = 0, height = 0;
    int64_t timestamp = 0;
    double pixelAspect = 1;
    std::vector<uint8_t> bgra;
};

void convertNv12(VideoFrame& frame, const uint8_t* luma, const uint8_t* chroma, size_t pitch,
    VideoArea area, bool bt709, bool fullRange);

enum class Playback : int { idle, loading, paused, playing, ended, failed };
struct PlaybackUpdate {
    uint32_t id = 0;
    Playback state = Playback::idle;
    double position = 0, duration = 0;
    long error = 0;
    std::shared_ptr<const VideoFrame> frame;
    uint64_t seekRevision = 0;
};

class Decoder {
    struct State;
    std::shared_ptr<State> state;
public:
    explicit Decoder(std::function<void()> notify);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    void load(uint32_t id, std::wstring url, double start, std::shared_ptr<Audio> audio);
    void clear();
    void play(bool enabled);
    uint64_t seek(double seconds);
    PlaybackUpdate take();
};

bool verifyHardwareDecode(const std::wstring& url);
}
