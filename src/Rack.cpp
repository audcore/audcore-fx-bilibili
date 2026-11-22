#include "Audio.h"
#include "VideoId.h"
#include <cstring>
#include "Assets.h"
#include "audcore_fx.h"
#include <limits>
#include <mutex>
#include <unordered_map>

namespace bilibili {
namespace {
std::mutex registryMutex;
std::unordered_map<uint32_t, std::weak_ptr<Audio>> registry;
uint32_t nextId = 0;

struct Rack {
    std::shared_ptr<Audio> audio = std::make_shared<Audio>();
    uint32_t id = 0;
    std::string saved;

    Rack() {
        std::lock_guard<std::mutex> lock(registryMutex);
        do { ++nextId; } while (nextId == 0 || registry.count(nextId));
        id = nextId;
        registry.emplace(id, audio);
    }
    ~Rack() {
        audio->playing.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(registryMutex);
        registry.erase(id);
    }
};

void* create() { try { return new Rack; } catch (...) { return nullptr; } }
void destroy(void* handle) { delete static_cast<Rack*>(handle); }
Audio& audio(void* handle) { return *static_cast<Rack*>(handle)->audio; }

int prepare(void* handle, uint32_t rate, uint32_t frames, uint32_t channels, uint32_t* latency) {
    if (rate < 8000 || rate > 384000 || !frames || !latency || channels < 1 || channels > 2) return 0;
    audio(handle).prepare(rate, channels);
    *latency = 0;
    return 1;
}
void process(void* handle, float* stereo, const float*, uint32_t frames, double) {
    audio(handle).mix(stereo, frames);
}
float get(void* handle, uint32_t index) {
    return index == 0 && audio(handle).bypass.load(std::memory_order_relaxed) ? 1.f : 0.f;
}
int set(void* handle, uint32_t index, float normalized) {
    if (index != 0 || !(normalized >= 0 && normalized <= 1)) return 0;
    audio(handle).bypass.store(normalized >= .5f, std::memory_order_relaxed);
    audio(handle).reset();
    return 1;
}
uint32_t save(void* handle, uint8_t* output, uint32_t room) {
    auto& state = static_cast<Rack*>(handle)->saved;
    if (!output) {
        try {
            const auto video = audio(handle).video();
            state.assign({3, static_cast<char>(get(handle, 0)), static_cast<char>(video.part & 255),
                static_cast<char>(video.part >> 8)});
            state += video.id;
            return static_cast<uint32_t>(state.size());
        } catch (...) { return 0; }
    }
    if (room < state.size()) return 0;
    std::memcpy(output, state.data(), state.size());
    return static_cast<uint32_t>(state.size());
}
int restore(void* handle, const uint8_t* input, uint32_t size) {
    try {
        if (!size) { audio(handle).select({}); return set(handle, 0, 0); }
        if (!input || size < 2 || input[1] > 1) return 0;
        const auto version = input[0];
        if ((version == 1 && size != 2) || (version == 2 && size > 23) ||
            (version == 3 && (size < 4 || size > 25)) || version < 1 || version > 3) return 0;
        const uint32_t prefix = version == 3 ? 4 : 2;
        const auto part = static_cast<uint16_t>(version == 3 ? input[2] | (input[3] << 8) : 1);
        const std::string_view id(reinterpret_cast<const char*>(input + prefix), size - prefix);
        if (part == 0 || part > 9999 || (id.empty() ? part != 1 : !validVideoId(id))) return 0;
        audio(handle).select({std::string(id), part});
        return set(handle, 0, static_cast<float>(input[1]));
    } catch (...) { return 0; }
}
uint32_t visual(void* handle, float* data, uint32_t size) {
    if (!data || size < 3) return 0;
    const auto id = static_cast<Rack*>(handle)->id;
    data[0] = static_cast<float>(id >> 16);
    data[1] = static_cast<float>(id & 0xffff);
    data[2] = static_cast<float>(audio(handle).rate.load(std::memory_order_acquire));
    return 3;
}
}

std::shared_ptr<Audio> audioFor(uint32_t id) {
    std::lock_guard<std::mutex> lock(registryMutex);
    const auto it = registry.find(id);
    return it == registry.end() ? nullptr : it->second.lock();
}
}

extern "C" AUDCORE_FX_EXPORT const AudcoreFxRack* audcore_fx_get_rack(uint32_t abi) {
    using namespace bilibili;
    static const AudcoreFxRack api{sizeof api, AUDCORE_FX_RACK_ABI_VERSION, "BILIBILI",
        "82D2F61FC33949E6812D012C191D5F30", BILIBILI_VERSION, create, destroy, prepare, process,
        get, set, save, restore, [](void*, int) {}, visual};
    return abi == AUDCORE_FX_RACK_ABI_VERSION ? &api : nullptr;
}

extern "C" AUDCORE_FX_EXPORT const char* audcore_fx_get_definition(uint32_t abi, uint32_t* size) {
    if (abi != 1 || !size) return nullptr;
    *size = sizeof(bilibili::definition) - 1;
    return bilibili::definition;
}



extern "C" AUDCORE_FX_EXPORT int audcore_fx_rack_action(void* handle, const char* command) {
    try {
        using namespace bilibili;
        const std::string_view text(command ? command : "");
        if (text.substr(0, 6) != "video:") return 0;
        const auto target = text.substr(6);
        const auto split = target.find(':');
        const auto id = target.substr(0, split);
        uint16_t part = 1;
        if (!id.empty() && !validVideoId(id)) return 0;
        if (split != target.npos) {
            const auto number = target.substr(split + 1);
            if (id.empty() || number.empty() || number.size() > 4) return 0;
            part = 0;
            for (const char digit : number) {
                if (digit < '0' || digit > '9') return 0;
                part = static_cast<uint16_t>(part * 10 + digit - '0');
            }
            if (part == 0) return 0;
        }
        audio(handle).select({std::string(id), part}, nullptr, true);
        return 1;
    } catch (...) { return 0; }
}

extern "C" AUDCORE_FX_EXPORT uint32_t audcore_fx_rack_get_text(
    void* handle, const char* parameter, char* output, uint32_t capacity) {
    try {
        if (!parameter || !output) return 0;
        auto selection = bilibili::audio(handle).video();
        std::string id;
        if (std::strcmp(parameter, "video_id") == 0) id = std::move(selection.id);
        else if (std::strcmp(parameter, "video_part") == 0) id = std::to_string(selection.part);
        else return 0;
        if (id.size() + 1 > capacity) return 0;
        std::memcpy(output, id.c_str(), id.size() + 1);
        return static_cast<uint32_t>(id.size() + 1);
    } catch (...) { return 0; }
}
