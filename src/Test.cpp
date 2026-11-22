#include "Audio.h"
#include "Api.h"
#include <windows.h>
#include <objbase.h>
#include "Decoder.h"
#include <string_view>
#include "Server.h"
#include <cmath>
#include <chrono>
#include <thread>
#include <iostream>

namespace {
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
    for (int block = 0; block < 3; ++block) audio.write(decoded.data(), 1024, 2);
    audio.mix(effectChain.data(), 128);
    for (size_t index = 0; index < effectChain.size(); index += 2) {
        if (std::abs(effectChain[index] - (channels == 1 ? .375f : .75f)) > .0001f ||
            std::abs(effectChain[index + 1] - (channels == 1 ? .375f : 0.f)) > .0001f) return false;
    }
    for (int block = 0; block < 400; ++block) {
        effectChain.fill(.25f);
        audio.mix(effectChain.data(), 128);
    }
    if (effectChain[0] != .25f || effectChain[1] != .25f) return false;
    audio.reset();
    audio.write(decoded.data(), 1024, 2);
    audio.mix(effectChain.data(), 128);
    return effectChain[0] == .25f && effectChain[1] == .25f;
}
}

int main(int argc, char** argv) {
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
    if (argc == 3 && std::string_view(argv[1]) == "--decode") {
        const std::string_view url(argv[2]);
        if (!bilibili::verifyHardwareDecode(std::wstring(url.begin(), url.end()))) return 3;
        const auto audio = std::make_shared<bilibili::Audio>();
        audio->prepare(48000, 2);
        bilibili::Decoder player([] {});
        player.load(1, std::wstring(url.begin(), url.end()), 10, audio);
        player.play(true);
        std::array<float, 256> pcm{};
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool picture = false, audible = false;
        double duration = 0;
        while (std::chrono::steady_clock::now() < deadline && !(picture && audible)) {
            const auto update = player.take();
            if (update.state == bilibili::Playback::failed) return 4;
            duration = update.duration;
            picture |= update.frame && !update.frame->nv12.empty() &&
                update.frame->bgra.size() == static_cast<size_t>(update.frame->width) * update.frame->height * 4 &&
                update.frame->bgra[3] == 255;
            pcm.fill(0.f);
            audio->mix(pcm.data(), 128);
            audible |= std::any_of(pcm.begin(), pcm.end(), [](float sample) { return std::abs(sample) > .001f; });
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
        if (!picture || !audible) return 5;
        std::cout << "Media Foundation produced D3D11 NV12 video and audible PCM in the rack mixer\n";

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
