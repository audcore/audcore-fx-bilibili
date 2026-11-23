#include "Transport.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Stream final : IBStream {
    std::vector<char> bytes;
    size_t position = 0;
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API queryInterface(const TUID, void** out) override { *out = nullptr; return kNoInterface; }
    tresult PLUGIN_API read(void* out, int32 size, int32* count) override {
        const auto n = std::min(size_t(size), bytes.size() - position);
        if (n) std::memcpy(out, bytes.data() + position, n);
        position += n;
        *count = int32(n);
        return n ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API write(void* data, int32 size, int32* count) override {
        const auto* first = static_cast<const char*>(data);
        bytes.insert(bytes.end(), first, first + size);
        *count = size;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64, int32, int64*) override { return kNotImplemented; }
    tresult PLUGIN_API tell(int64* out) override { *out = int64(position); return kResultOk; }
};

struct StalledPeer final : IConnectionPoint {
    std::atomic<bool> stall{false};
    std::atomic<int> frames{0};
    std::promise<void> entered;
    std::promise<void> resume;
    std::shared_future<void> ready = resume.get_future().share();
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API queryInterface(const TUID, void** out) override { *out = nullptr; return kNoInterface; }
    tresult PLUGIN_API connect(IConnectionPoint*) override { return kNotImplemented; }
    tresult PLUGIN_API disconnect(IConnectionPoint*) override { return kNotImplemented; }
    tresult PLUGIN_API notify(IMessage* message) override {
        audcore::nativevst::Packet packet;
        if (!audcore::nativevst::receive(message, packet)) return kResultFalse;
        frames.store(packet.count);
        if (stall.exchange(false)) {
            entered.set_value();
            ready.wait();
        }
        return kResultOk;
    }
};

int main(int argc, char** argv) try {
    check(argc == 3, "Expected a VST3 path and version");
    const auto dll = LoadLibraryA(argv[1]);
    check(dll, "Load Bilibili VST3");
    const auto init = reinterpret_cast<bool(*)()>(GetProcAddress(dll, "InitDll"));
    const auto exit = reinterpret_cast<bool(*)()>(GetProcAddress(dll, "ExitDll"));
    const auto entry = reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(GetProcAddress(dll, "GetPluginFactory"));
    check(init && exit && entry && init(), "VST3 entry points");
    auto* factory = entry();
    IPluginFactory2* infoFactory = nullptr;
    PClassInfo2 info{};
    check(factory && factory->countClasses() == 2 &&
        factory->queryInterface(IPluginFactory2::iid.toTUID(), reinterpret_cast<void**>(&infoFactory)) == kResultOk &&
        infoFactory->getClassInfo2(0, &info) == kResultOk && std::strcmp(info.name, "Bilibili") == 0 &&
        std::strcmp(info.version, argv[2]) == 0, "VST3 product identity and version");
    infoFactory->release();

    IComponent* component = nullptr;
    IAudioProcessor* audio = nullptr;
    check(factory->createInstance(info.cid, IComponent::iid.toTUID(), reinterpret_cast<void**>(&component)) == kResultOk &&
        component->initialize(nullptr) == kResultOk &&
        component->queryInterface(IAudioProcessor::iid.toTUID(), reinterpret_cast<void**>(&audio)) == kResultOk, "Processor lifecycle");
    TUID controllerId{};
    IEditController* controller = nullptr;
    check(component->getControllerClassId(controllerId) == kResultOk &&
        factory->createInstance(controllerId, IEditController::iid.toTUID(), reinterpret_cast<void**>(&controller)) == kResultOk &&
        controller->initialize(nullptr) == kResultOk && controller->getParameterCount() == 2, "Controller lifecycle");
    ParameterInfo parameter{};
    check(controller->getParameterInfo(0, parameter) == kResultOk &&
        parameter.stepCount == 1 && (parameter.flags & ParameterInfo::kIsBypass), "Host bypass parameter");
    ParameterInfo volume{};
    check(controller->getParameterInfo(1, volume) == kResultOk && volume.defaultNormalizedValue == .5 &&
        controller->getParamNormalized(volume.id) == .5, "Host volume parameter defaults to 50 percent");
    auto* view = controller->createView(ViewType::kEditor);
    ViewRect rect{};
    check(view && view->getSize(&rect) == kResultOk && rect.getWidth() == 720 && rect.getHeight() == 405 &&
        view->canResize() == kResultTrue, "Resizable native editor");
    ViewRect custom{0, 0, 640, 480};
    check(view->checkSizeConstraint(&custom) == kResultOk && custom.getWidth() == 640 && custom.getHeight() == 480,
        "Custom aspect ratio remains available");
    view->release();

    Stream selection;
    check(component->getState(&selection) == kResultOk, "Read initial state");
    const std::array<char, 4> signature{'R','A','C','K'};
    const auto chunk = std::search(selection.bytes.begin(), selection.bytes.end(), signature.begin(), signature.end());
    check(chunk != selection.bytes.end() && selection.bytes.end() - chunk == 13, "Rack state framing");
    constexpr char id[] = "BV1B7411m7LV";
    *(chunk + 4) += sizeof(id) - 1;
    *(chunk + 9) = 1;
    *(chunk + 10) = 2;
    *(chunk + 12) = 73;
    selection.bytes.insert(selection.bytes.end(), id, id + sizeof(id) - 1);
    check(component->setState(&selection) == kResultOk, "Restore video and part");
    Stream saved;
    check(component->getState(&saved) == kResultOk && saved.bytes.size() >= 17 &&
        std::equal(saved.bytes.end() - 17, saved.bytes.end(), selection.bytes.end() - 17),
        "Persist video ID, part, bypass and volume in VST3 state");
    check(controller->setComponentState(&saved) == kResultOk && controller->getParamNormalized(parameter.id) == 1 &&
        controller->getParamNormalized(volume.id) > .7299 && controller->getParamNormalized(volume.id) < .7301,
        "Controller reads host parameter state");
    selection.position = 0;
    selection.bytes.pop_back();
    check(component->setState(&selection) != kResultOk, "Reject truncated state");

    constexpr int frames = 128;
    for (const auto count : {1, 2}) {
        auto arrangement = count == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        ProcessSetup setup{kRealtime, kSample32, frames, 48000};
        check(audio->setBusArrangements(&arrangement, 1, &arrangement, 1) == kResultOk &&
            audio->setupProcessing(setup) == kResultOk && component->setActive(true) == kResultOk &&
            audio->setProcessing(true) == kResultOk && audio->getLatencySamples() == 0, "Prepare mono and stereo");
        std::array<float, frames> left{}, right{}, outLeft{}, outRight{};
        left.fill(.25f);
        right.fill(-.5f);
        float* inputs[]{left.data(), right.data()};
        float* outputs[]{outLeft.data(), outRight.data()};
        AudioBusBuffers in, out;
        in.numChannels = out.numChannels = count;
        in.channelBuffers32 = inputs;
        out.channelBuffers32 = outputs;
        ProcessData data;
        data.numSamples = frames;
        data.numInputs = data.numOutputs = 1;
        data.inputs = &in;
        data.outputs = &out;
        check(audio->process(data) == kResultOk && outLeft == left && (count == 1 || outRight == right),
            "Preserve input audio on bypass");
        if (count == 1) {
            StalledPeer peer;
            IConnectionPoint* source = nullptr;
            check(component->queryInterface(IConnectionPoint::iid.toTUID(), reinterpret_cast<void**>(&source)) == kResultOk &&
                source->connect(&peer) == kResultOk, "Connect telemetry");
            audcore::nativevst::Message command;
            command.packet.command = 2;
            command.packet.action = 1;
            check(source->notify(&command) == kResultOk && peer.frames.load() == 3, "Bind editor before processing starts");
            auto entered = peer.entered.get_future();
            peer.stall.store(true);
            for (int block = 0; block < 16; ++block) audio->process(data);
            const bool stalled = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            auto processed = std::async(std::launch::async, [&] {
                for (int block = 0; block < 160; ++block)
                    if (audio->process(data) != kResultOk) return false;
                return true;
            });
            const bool immediate = processed.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            peer.resume.set_value();
            const bool ok = processed.get();
            source->disconnect(&peer);
            source->release();
            check(stalled && immediate && ok, "Process audio while a full telemetry queue cannot reach the editor");
        }
        check(audio->setProcessing(false) == kResultOk && component->setActive(false) == kResultOk, "Deactivate processor");
    }
    check(controller->terminate() == kResultOk && component->terminate() == kResultOk, "Terminate plugin");
    audio->release();
    component->release();
    controller->release();
    factory->release();
    check(exit() && FreeLibrary(dll), "Release VST3");
    std::cout << "Bilibili VST3 factory, host state, mono/stereo and zero latency validated\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
