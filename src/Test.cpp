#include "Audio.h"
#include "Api.h"
#include <windows.h>
#include <objbase.h>
#include "Decoder.h"
#include <string_view>
#include "Server.h"
#include "Text.h"
#include <cmath>
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>

namespace {
bool selectionTest() {
    bilibili::Audio audio;
    unsigned first = 0, second = 0;
    const bilibili::Selection target{"BV1F8yVzKETm", 2};
    if (!audio.watchSelection(&first, [&] { ++first; }).id.empty()) return false;
    if (!audio.select(target, &first) || first != 0) return false;
    if (!(audio.watchSelection(&second, [&] { ++second; }) == target)) return false;
    if (audio.select(target, nullptr, true) || first != 1 || second != 1) return false;
    audio.unwatchSelection(&first);
    if (!audio.select({"AV98647868", 1}) || first != 1 || second != 2) return false;
    return true;
}

bool frameCropTest() {
    constexpr size_t pitch = 12;
    constexpr unsigned surfaceHeight = 8;
    std::array<uint8_t, pitch * surfaceHeight * 3 / 2> pixels;
    pixels.fill(255);
    auto* chroma = pixels.data() + pitch * surfaceHeight;
    std::fill(chroma, pixels.data() + pixels.size(), uint8_t{128});
    // The visible picture starts off the origin. Each row and column is
    // distinguishable, including the last picture row next to the padding.
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 5; ++x) pixels[(y + 1) * pitch + x + 1] = static_cast<uint8_t>(20 + y * 30 + x);
    chroma[2 * pitch + 4] = 150;
    bilibili::VideoFrame frame;
    bilibili::convertNv12(frame, pixels.data(), chroma, pitch, {1, 1, 5, 4}, true, true);
    if (frame.width != 5 || frame.height != 4 || frame.bgra.size() != 80) return false;
    for (unsigned y = 0; y < frame.height; ++y) {
        for (unsigned x = 0; x < frame.width; ++x) {
            const auto* pixel = frame.bgra.data() + (y * frame.width + x) * 4;
            const unsigned expected = 20 + y * 30 + x;
            const bool blue = y == 3 && x >= 3;
            if (pixel[3] != 255 || pixel[2] != expected ||
                pixel[0] != expected + (blue ? 41 : 0) || pixel[1] != expected - (blue ? 4 : 0)) return false;
        }
    }
    return true;
}

bool titleTextTest() {
    for (const unsigned scale : {1u, 2u}) {
        const bilibili::TextStyle style{180 * scale, 50 * scale, 14.f * scale, 0x8090c0ff, true, false, true};
        const auto first = bilibili::rasterizeText(L"【\U0001D7D2K】周杰伦", style);
        const auto second = bilibili::rasterizeText(L"【\U0001D7D6K】周杰伦", style);
        if (first.bytes.size() != static_cast<size_t>(style.width) * style.height * 4 ||
            first.bytes.size() != second.bytes.size() || first.bytes == second.bytes) return false;
        int top = style.height, bottom = 0;
        for (unsigned y = 0; y < style.height; ++y) {
            for (unsigned x = 0; x < style.width; ++x) {
                const auto pixel = &first.bytes[(static_cast<size_t>(y) * style.width + x) * 4];
                if (pixel[3] > 128 || pixel[0] > pixel[3] || pixel[1] > pixel[3] || pixel[2] > pixel[3]) return false;
                if (pixel[3] < 32) continue;
                top = std::min(top, static_cast<int>(y));
                bottom = std::max(bottom, static_cast<int>(y) + 1);
            }
        }
        if (bottom <= top || std::abs(top + bottom + 2 * first.offsetY - static_cast<int>(style.height)) > 1) return false;
    }
    return true;
}

bool audioTest(uint32_t rate, uint32_t channels) {
    bilibili::Audio audio;
    audio.prepare(rate, channels);
    audio.playing.store(true);
    std::array<float, 256> effectChain;
    effectChain.fill(.25f);
    audio.mix(effectChain.data(), 128);
    if (effectChain[0] != .25f || effectChain[1] != .25f) return false;
    std::array<float, 2048> decoded{};
    for (size_t index = 0; index < decoded.size(); index += 2) {
        decoded[index] = .5f;
        decoded[index + 1] = -.25f;
    }
    for (int block = 0; block < 3; ++block) audio.write(decoded.data(), 1024, 2, 48000);
    audio.mix(effectChain.data(), 128);
    const auto gain = std::pow(10.f, -.5f);
    for (size_t index = 0; index < effectChain.size(); index += 2) {
        if (std::abs(effectChain[index] - (.25f + (channels == 1 ? .125f : .5f) * gain)) > .0001f ||
            std::abs(effectChain[index + 1] - (.25f + (channels == 1 ? .125f : -.25f) * gain)) > .0001f) return false;
    }
    for (int block = 0; block < 400; ++block) {
        effectChain.fill(.25f);
        audio.mix(effectChain.data(), 128);
    }
    if (effectChain[0] != .25f || effectChain[1] != .25f) return false;
    audio.reset();
    audio.write(decoded.data(), 1024, 2, 48000);
    audio.mix(effectChain.data(), 128);
    return effectChain[0] == .25f && effectChain[1] == .25f;
}

bool volumeTest() {
    bilibili::Audio audio;
    audio.prepare(48000, 2);
    audio.playing.store(true);
    std::array<float, 2 * 4096> source;
    source.fill(.25f);
    std::array<float, 2 * 512> output;
    output.fill(.1f);
    audio.volume.store(100);
    audio.mix(output.data(), 512);
    audio.write(source.data(), 4096, 2, 48000);
    audio.mix(output.data(), 512);
    if (std::abs(output.back() - .35f) > .0001f) return false;
    audio.volume.store(0);
    output.fill(.1f);
    audio.mix(output.data(), 512);
    return output.front() > .34f && output.front() < .35f && output.back() == .1f;
}

bool formatTransitionTest(bool monoFirst) {
    bilibili::Audio audio;
    audio.prepare(48000, 2);
    audio.volume.store(100);
    audio.playing.store(true);
    std::array<float, 2400> mono;
    mono.fill(.25f);
    std::array<float, 9600> stereo;
    for (size_t i = 0; i < stereo.size(); i += 2) { stereo[i] = -.5f; stereo[i + 1] = .125f; }
    std::array<float, 256> output{};
    audio.mix(output.data(), 128);
    const auto writeMono = [&] { audio.write(mono.data(), 2400, 1, 24000); };
    const auto writeStereo = [&] { audio.write(stereo.data(), 4800, 2, 48000); };
    if (monoFirst) { writeMono(); writeStereo(); } else { writeStereo(); writeMono(); }
    unsigned monoFrames = 0, stereoFrames = 0;
    for (int block = 0; block < 90; ++block) {
        output.fill(0.f);
        audio.mix(output.data(), 128);
        for (size_t i = 0; i < output.size(); i += 2) {
            if (output[i] == .25f && output[i + 1] == .25f) ++monoFrames;
            else if (output[i] == -.5f && output[i + 1] == .125f) ++stereoFrames;
            else if (output[i] != 0 || output[i + 1] != 0) return false;
        }
    }
    // Both queued sections are 100 ms. The queue correction is at most 1%.
    return monoFrames >= 4740 && monoFrames <= 4860 && stereoFrames >= 4740 && stereoFrames <= 4860;
}

int checkMonoPlayback(const std::wstring& url, std::ostream* capture, const std::wstring& stereoUrl = {}) {
    using Clock = std::chrono::steady_clock;
    auto audio = std::make_shared<bilibili::Audio>();
    audio->prepare(48000, 2);
    audio->volume.store(100);
    bilibili::Decoder player([] {});
    player.load(1, url, 0, audio);
    player.play(true);
    const auto verify = [&](uint32_t id, uint64_t revision, int blocks, bool mono, std::ostream* record) {
        std::array<float, 960> pcm{};
        auto next = Clock::now();
        unsigned signalFrames = 0;
        unsigned stereoFrames = 0;
        bool picture = false;
        for (int block = 0; block < blocks; ++block) {
            const auto update = player.take();
            if (update.state == bilibili::Playback::failed) return false;
            picture |= update.id == id && update.seekRevision == revision && static_cast<bool>(update.frame);
            pcm.fill(0.f);
            audio->mix(pcm.data(), 480);
            for (size_t i = 0; i < pcm.size(); i += 2) {
                if (mono && pcm[i] != pcm[i + 1]) return false;
                if (std::abs(pcm[i] - pcm[i + 1]) > .001f) ++stereoFrames;
                if (std::abs(pcm[i]) > .001f) ++signalFrames;
            }
            if (record) record->write(reinterpret_cast<const char*>(pcm.data()), sizeof pcm);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
        return picture && signalFrames > 12000 && (mono || stereoFrames > 12000) && (!record || *record);
    };
    if (!verify(1, 0, 320, true, capture)) return 15;
    if (!verify(1, player.seek(12.), 220, true, nullptr)) return 16;
    std::cout << "24 kHz mono source stays mono when mixed at 48 kHz, including after seek\n";
    if (!stereoUrl.empty()) {
        player.load(2, stereoUrl, 2, audio);
        player.play(true);
        if (!verify(2, 0, 260, false, nullptr)) return 19;
        player.load(3, url, 0, audio);
        player.play(true);
        if (!verify(3, 0, 320, true, nullptr)) return 20;
        std::cout << "Switching mono to stereo and back keeps the correct channel layout\n";
    }
    return 0;
}
}

int main(int argc, char** argv) {
    if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--check-mono") {
        const std::string_view url(argv[2]);
        std::ofstream capture;
        if (argc == 4) {
            capture.open(argv[3], std::ios::binary);
            if (!capture) return 17;
        }
        return checkMonoPlayback(std::wstring(url.begin(), url.end()), argc == 4 ? &capture : nullptr);
    }
    if (argc == 4 && std::string_view(argv[1]) == "--check-audio-switch") {
        const std::string_view mono(argv[2]), stereo(argv[3]);
        return checkMonoPlayback(std::wstring(mono.begin(), mono.end()), nullptr, std::wstring(stereo.begin(), stereo.end()));
    }
    if (argc == 3 && std::string_view(argv[1]) == "--api") {
        const std::string_view url(argv[2]);
        const std::wstring base(url.begin(), url.end());
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) return 6;
        const auto target = bilibili::parseTarget(L"https://www.bilibili.com/video/BV1F8yVzKETm/?p=2");
        const auto page = bilibili::search(base, L"音乐现场", 1);
        const auto cover = page && !page->rows.empty() ? bilibili::loadCover(base, page->rows.front().cover) : nullptr;
        const auto video = target ? bilibili::loadVideo(base, *target) : std::nullopt;
        const auto comments = bilibili::loadComments(base, 168325345);
        CoUninitialize();
        if (!target || target->part != 2 || !cover || !video || video->parts.size() < 2 || video->target.part != 2 ||
            video->media.empty() || comments.empty() || cover->bytes.size() != static_cast<size_t>(cover->width) * cover->height * 4)
            return 7;
        for (size_t i = 0; i < cover->bytes.size(); i += 4) {
            const auto& pixel = cover->bytes;
            if (pixel[i] > pixel[i+3] || pixel[i+1] > pixel[i+3] || pixel[i+2] > pixel[i+3]) return 8;
        }
        std::cout << "Native search, premultiplied covers, part selection and danmaku passed\n";
        return 0;
    }
    if ((argc == 3 || argc == 6) && std::string_view(argv[1]) == "--decode") {
        const std::string_view url(argv[2]);
        if (!bilibili::verifyHardwareDecode(std::wstring(url.begin(), url.end()))) return 3;
        const auto audio = std::make_shared<bilibili::Audio>();
        audio->prepare(48000, 2);
        bilibili::Decoder player([] {});
        const double start = argc == 6 ? std::stod(argv[3]) : 10.;
        const auto expectedWidth = argc == 6 ? std::stoul(argv[4]) : 0;
        const auto expectedHeight = argc == 6 ? std::stoul(argv[5]) : 0;
        player.load(1, std::wstring(url.begin(), url.end()), start, audio);
        player.play(true);
        std::array<float, 256> pcm{};
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool picture = false, audible = false;
        double duration = 0;
        while (std::chrono::steady_clock::now() < deadline && !(picture && audible)) {
            const auto update = player.take();
            if (update.state == bilibili::Playback::failed) return 4;
            duration = update.duration;
            if (update.frame && expectedWidth && (update.frame->width != expectedWidth || update.frame->height != expectedHeight)) {
                std::cerr << "Unexpected visible frame size " << update.frame->width << 'x' << update.frame->height << '\n';
                return 12;
            }
            picture |= update.frame && !update.frame->bgra.empty() &&
                update.frame->bgra.size() == static_cast<size_t>(update.frame->width) * update.frame->height * 4 &&
                update.frame->bgra[3] == 255;
            pcm.fill(0.f);
            audio->mix(pcm.data(), 128);
            audible |= std::any_of(pcm.begin(), pcm.end(), [](float sample) { return std::abs(sample) > .001f; });
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
        if (!picture || !audible) return 5;
        std::cout << "Media Foundation produced D3D11 NV12 video and audible PCM in the rack mixer\n";
        if (expectedWidth) {
            std::cout << "Visible picture matches " << expectedWidth << 'x' << expectedHeight << " at " << start << "s\n";
            return 0;
        }

        if (duration <= 25) return 6;
        const double target = duration * .75;
        const auto revision = player.seek(target);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto update = player.take();
            if (update.seekRevision == revision) {
                if (update.state == bilibili::Playback::failed) return 7;
                if (update.frame) {
                    const double frameTime = static_cast<double>(update.frame->timestamp) / 1000000;
                    if (std::abs(frameTime - target) > .3 || std::abs(update.position - target) > .3) {
                        std::cerr << "Seek target " << target << ", frame " << frameTime << ", position " << update.position << '\n';
                        return 8;
                    }
                    std::cout << "Media Foundation sought to a new range without advancing during preroll\n";
                    return 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        return 9;
    }
    if (argc == 1) {
        if (!formatTransitionTest(true) || !formatTransitionTest(false)) {
            std::cerr << "Queued mono and stereo audio lost their original time scale during a format change\n";
            return 18;
        }
        std::cout << "24/48 kHz transitions preserve frame duration and channel layout in both directions\n";
        if (!volumeTest()) {
            std::cerr << "Source volume changed upstream gain or did not ramp to silence\n";
            return 14;
        }
        std::cout << "Volume preserves upstream audio and ramps to silence\n";
        if (!selectionTest()) {
            std::cerr << "Video selection was not published correctly to open editors\n";
            return 13;
        }
        std::cout << "Video selection reaches open editors and unsubscribes on close\n";
        if (!frameCropTest()) {
            std::cerr << "NV12 picture crop includes padding or samples the wrong chroma cells\n";
            return 11;
        }
        std::cout << "NV12 visible frame excludes the coded padding and preserves its last row\n";
        if (!titleTextTest()) {
            std::cerr << "Title fallback, centering or premultiplied alpha failed\n";
            return 10;
        }
        std::cout << "Title fallback, pixel centering and premultiplied alpha passed at 100% and 200% scale\n";
        for (const auto rate : {16000u, 44100u, 48000u, 96000u, 192000u}) {
            if (!audioTest(rate, 1) || !audioTest(rate, 2)) return 1;
        }
        std::cout << "PCM joins the effect chain in mono and stereo; underruns preserve upstream audio\n";
        return 0;
    }
    auto server = bilibili::startServer();
    if (!server) return 2;
    const auto url = bilibili::panelUrl(*server);
    for (const wchar_t c : url) std::cout << static_cast<char>(c);
    std::cout << std::endl;
    std::cin.get();
    bilibili::stopServer(*server);
    return 0;
}
