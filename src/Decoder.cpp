#include "Decoder.h"
#include "Audio.h"
#include "Mp4Stream.h"
#include <windows.h>
#include <objbase.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace bilibili {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD anyStream = static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM);
constexpr double ticks = 10000000.0;
constexpr HRESULT hardwareUnavailable = DXGI_ERROR_UNSUPPORTED;

class WindowsMedia {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
public:
    const HRESULT status = SUCCEEDED(com) ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : com;
    ~WindowsMedia() {
        if (SUCCEEDED(status)) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

void setPlaying(const std::shared_ptr<Audio>& audio, bool playing) {
    if (!audio) return;
    if (audio->playing.exchange(playing, std::memory_order_acq_rel) != playing) audio->reset();
}

class File {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<IMFSourceReader> reader;
    uint32_t width = 0, height = 0;
    bool bt709 = true, fullRange = false;

    bool getDevice() {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapters1(index, &adapter))) return false;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) continue;
            ComPtr<ID3D11VideoDevice> video;
            if (SUCCEEDED(device.As(&video)) && video->GetVideoDecoderProfileCount()) break;
            context.Reset();
            device.Reset();
        }
        ComPtr<ID3D11Multithread> multithread;
        if (FAILED(context.As(&multithread))) return false;
        multithread->SetMultithreadProtected(TRUE);
        UINT token = 0;
        return SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager)) &&
            SUCCEEDED(manager->ResetDevice(device.Get(), token));
    }

public:
    double duration = 0;
    uint32_t channels = 0, sampleRate = 0;

    HRESULT open(const std::wstring& url) {
        if (!getDevice()) return hardwareUnavailable;
        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(&attributes, 4);
        if (FAILED(result)) return result;
        attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.Get());
        attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
        ComPtr<IMFByteStream> stream;
        if (FAILED(result = openMp4Stream(url, &stream))) return result;
        result = MFCreateSourceReaderFromByteStream(stream.Get(), attributes.Get(), &reader);
        if (FAILED(result)) return result;
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (FAILED(result = reader->SetStreamSelection(videoStream, TRUE))) return result;

        ComPtr<IMFMediaType> type;
        if (FAILED(result = MFCreateMediaType(&type))) return result;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(reader->SetCurrentMediaType(videoStream, nullptr, type.Get()))) return hardwareUnavailable;
        if (FAILED(result = updateVideoType())) return result;

        if (SUCCEEDED(reader->SetStreamSelection(audioStream, TRUE))) {
            type.Reset();
            if (FAILED(result = MFCreateMediaType(&type))) return result;
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            if (FAILED(result = reader->SetCurrentMediaType(audioStream, nullptr, type.Get()))) return result;
            if (FAILED(result = reader->GetCurrentMediaType(audioStream, &type))) return result;
            if (FAILED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
                FAILED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate)) ||
                (channels != 1 && channels != 2) || sampleRate < 8000 || sampleRate > 96000) return MF_E_INVALIDMEDIATYPE;
        }
        PROPVARIANT length{};
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length)) &&
            length.vt == VT_UI8) duration = static_cast<double>(length.uhVal.QuadPart) / ticks;
        PropVariantClear(&length);
        return S_OK;
    }

    HRESULT updateVideoType() {
        ComPtr<IMFMediaType> type;
        auto result = reader->GetCurrentMediaType(videoStream, &type);
        if (FAILED(result)) return result;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12 ||
            FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
            !width || !height || width > 3840 || height > 2160 || (width & 1) || (height & 1))
            return MF_E_INVALIDMEDIATYPE;
        UINT32 tag = 0;
        bt709 = FAILED(type->GetUINT32(MF_MT_YUV_MATRIX, &tag)) ? height >= 720 : tag == MFVideoTransferMatrix_BT709;
        fullRange = SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &tag)) && tag == MFNominalRange_0_255;
        return S_OK;
    }

    HRESULT seek(double position) {
        PROPVARIANT time{};
        time.vt = VT_I8;
        time.hVal.QuadPart = static_cast<LONGLONG>(position * ticks);
        return reader->SetCurrentPosition(GUID_NULL, time);
    }

    struct Sample {
        ComPtr<IMFSample> payload;
        DWORD stream = 0;
        LONGLONG timestamp = 0;
        bool video = false;
        bool end = false;
    };

    HRESULT read(bool onlyVideo, Sample& sample) {
        DWORD flags = 0;
        auto result = reader->ReadSample(onlyVideo ? videoStream : anyStream, 0, &sample.stream, &flags,
            &sample.timestamp, &sample.payload);
        if (FAILED(result)) return result;
        if (flags & MF_SOURCE_READERF_ERROR) return E_FAIL;
        sample.end = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        ComPtr<IMFMediaType> type;
        GUID major{};
        if ((sample.payload || sample.end) && SUCCEEDED(reader->GetCurrentMediaType(sample.stream, &type)) &&
            SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major))) sample.video = major == MFMediaType_Video;
        if (sample.video && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) return updateVideoType();
        return S_OK;
    }

    HRESULT frame(const Sample& sample, VideoFrame& output) {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> surface;
        ComPtr<ID3D11Texture2D> texture;
        UINT plane = 0;
        if (FAILED(sample.payload->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&surface)) ||
            FAILED(surface->GetResource(IID_PPV_ARGS(&texture))) || FAILED(surface->GetSubresourceIndex(&plane)))
            return hardwareUnavailable;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        // A system-memory frame or a GPU upload from a software decoder is insufficient.
        if (desc.Format != DXGI_FORMAT_NV12 || !(desc.BindFlags & D3D11_BIND_DECODER) ||
            width > desc.Width || height > desc.Height) return hardwareUnavailable;
        D3D11_TEXTURE2D_DESC old{};
        if (staging) staging->GetDesc(&old);
        if (!staging || desc.Width != old.Width || desc.Height != old.Height) {
            desc.ArraySize = 1;
            desc.MipLevels = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            staging.Reset();
            const HRESULT result = device->CreateTexture2D(&desc, nullptr, &staging);
            if (FAILED(result)) return result;
        }
        output.width = width;
        output.height = height;
        output.timestamp = sample.timestamp / 10;
        output.bt709 = bt709;
        output.fullRange = fullRange;
        output.nv12.resize(static_cast<size_t>(width) * height * 3 / 2);
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), plane, nullptr);
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(result)) return result;
        const auto* bytes = static_cast<const uint8_t*>(map.pData);
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(output.nv12.data() + static_cast<size_t>(row) * width, bytes + static_cast<size_t>(row) * map.RowPitch, width);
        bytes += static_cast<size_t>(desc.Height) * map.RowPitch;
        for (uint32_t row = 0; row < height / 2; ++row)
            std::memcpy(output.nv12.data() + static_cast<size_t>(height + row) * width,
                bytes + static_cast<size_t>(row) * map.RowPitch, width);
        context->Unmap(staging.Get(), 0);
        return S_OK;
    }

    HRESULT pcm(const Sample& sample, const std::shared_ptr<Audio>& audio, double skip) const {
        if (!channels || !audio) return S_OK;
        ComPtr<IMFMediaBuffer> buffer;
        auto result = sample.payload->ConvertToContiguousBuffer(&buffer);
        if (FAILED(result)) return result;
        BYTE* bytes = nullptr;
        DWORD length = 0;
        result = buffer->Lock(&bytes, nullptr, &length);
        if (FAILED(result)) return result;
        const auto frames = static_cast<uint32_t>(length / (sizeof(float) * channels));
        const auto offset = std::min(frames, static_cast<uint32_t>(std::max(0.0, skip) * sampleRate));
        if (frames > offset) audio->write(reinterpret_cast<const float*>(bytes) + offset * channels, frames - offset, channels);
        buffer->Unlock();
        return S_OK;
    }
};

struct Request {
    uint32_t id = 0;
    uint64_t file = 0, seek = 0, version = 0;
    std::wstring url;
    double position = 0;
    std::shared_ptr<Audio> audio;
    bool playing = false, stop = false;
};
}

struct Decoder::State {
    std::mutex mutex;
    std::condition_variable changed;
    Request wanted;
    PlaybackUpdate latest;
    bool posted = false;
    std::function<void()> notify;

    explicit State(std::function<void()> callback) : notify(std::move(callback)) {}

    void publish(PlaybackUpdate update) {
        std::lock_guard<std::mutex> lock(mutex);
        if (wanted.stop || update.id != wanted.id) return;
        if (!update.frame && latest.id == update.id) update.frame = std::move(latest.frame);
        latest = std::move(update);
        if (!posted) { posted = true; notify(); }
    }

    void edit(const std::function<void(Request&)>& update) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            update(wanted);
            ++wanted.version;
        }
        changed.notify_one();
    }

    Request request() {
        std::lock_guard<std::mutex> lock(mutex);
        return wanted;
    }

    bool wait(uint64_t version, Clock::duration timeout = Clock::duration::max()) {
        std::unique_lock<std::mutex> lock(mutex);
        const auto predicate = [&] { return wanted.version != version; };
        if (timeout == Clock::duration::max()) { changed.wait(lock, predicate); return true; }
        return changed.wait_for(lock, timeout, predicate);
    }

    void run() {
        WindowsMedia media;
        Request active;
        std::unique_ptr<File> file;
        File::Sample sample;
        double position = 0;
        double seekTo = 0;
        auto origin = Clock::now();
        auto updateTime = origin;
        bool playing = false;
        bool shown = false;
        bool ended = false;
        bool firstVideoEnded = false;
        bool firstAudioEnded = false;
        auto status = [&](Playback value, HRESULT error = S_OK, std::shared_ptr<const VideoFrame> frame = {}) {
            publish({active.id, value, position, file ? file->duration : 0, error, std::move(frame)});
        };
        auto fail = [&](HRESULT error) {
            setPlaying(active.audio, false);
            status(Playback::failed, error);
            playing = false;
            file.reset();
        };
        auto updatePosition = [&] {
            if (!playing) return;
            position = std::max(seekTo, std::chrono::duration<double>(Clock::now() - origin).count());
            if (file->duration > 0) position = std::min(position, file->duration);
        };

        for (;;) {
            const auto incoming = request();
            if (incoming.stop) break;
            if (incoming.file != active.file) {
                setPlaying(active.audio, false);
                active = incoming;
                file.reset();
                sample = {};
                shown = ended = playing = firstVideoEnded = firstAudioEnded = false;
                position = seekTo = active.position;
                if (active.url.empty()) { status(Playback::idle); wait(active.version); continue; }
                status(Playback::loading);
                file = std::make_unique<File>();
                const auto opened = SUCCEEDED(media.status) ? file->open(active.url) : media.status;
                if (FAILED(opened)) { fail(opened); wait(active.version); continue; }
                if (active.audio && file->sampleRate) {
                    active.audio->sourceRate.store(file->sampleRate, std::memory_order_release);
                    active.audio->reset();
                }
                if (file->duration > 0) position = seekTo = std::min(seekTo, std::max(0., file->duration - 0.05));
                if (seekTo > 0) {
                    const auto result = file->seek(seekTo);
                    if (FAILED(result)) { fail(result); continue; }
                }
                continue;
            }
            if (!file) { active = incoming; wait(active.version); continue; }
            if (incoming.seek != active.seek || (incoming.playing && incoming.version != active.version && ended)) {
                setPlaying(active.audio, false);
                if (active.audio) active.audio->reset();
                const double target = incoming.seek != active.seek ? incoming.position : 0;
                position = seekTo = std::clamp(target, 0., file->duration > 0 ? std::max(0., file->duration - .05) : target);
                const auto result = file->seek(seekTo);
                active = incoming;
                if (FAILED(result)) { fail(result); continue; }
                playing = shown = ended = firstVideoEnded = firstAudioEnded = false;
                sample = {};
            } else active = incoming;
            if (!ended && playing != active.playing) {
                updatePosition();
                playing = active.playing;
                if (playing) origin = Clock::now() - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(position));
                setPlaying(active.audio, playing);
                if (shown) status(playing ? Playback::playing : Playback::paused);
            }
            if (ended || (!playing && shown)) { wait(active.version); continue; }
            if (!sample.payload) {
                sample = {};
                const auto result = file->read(!playing, sample);
                if (FAILED(result)) { fail(result); continue; }
                if (sample.end) {
                    // A stream can end before the other stream. Read the remainder before draining.
                    const bool isVideo = !playing || sample.video;
                    if (isVideo) firstVideoEnded = true;
                    else firstAudioEnded = true;
                    if (!playing || (firstVideoEnded && (firstAudioEnded || !file->channels)) ||
                        (file->duration > 0 && position >= file->duration - .05)) {
                        if (playing && file->duration > position) {
                            const auto delay = std::chrono::duration<double>(std::min(.15, file->duration - position));
                            if (wait(active.version, std::chrono::duration_cast<Clock::duration>(delay))) continue;
                        }
                        setPlaying(active.audio, false);
                        playing = false;
                        ended = true;
                        position = file->duration;
                        status(Playback::ended);
                    }
                    continue;
                }
                if (!sample.payload) continue;
            }
            updatePosition();
            const auto timestamp = static_cast<double>(sample.timestamp) / ticks;
            const auto lead = sample.video ? 0.01 : 0.098;
            if (playing && timestamp > position + lead) {
                const auto delay = std::chrono::duration<double>(std::min(.05, timestamp - position - lead));
                if (wait(active.version, std::chrono::duration_cast<Clock::duration>(delay))) continue;
                if (Clock::now() - updateTime > std::chrono::milliseconds(120)) {
                    status(Playback::playing);
                    updateTime = Clock::now();
                }
                continue;
            }
            HRESULT result = S_OK;
            if (sample.video) {
                if (timestamp + .03 >= seekTo && (!playing || timestamp + .18 >= position)) {
                    auto frame = std::make_shared<VideoFrame>();
                    result = file->frame(sample, *frame);
                    if (SUCCEEDED(result)) {
                        shown = true;
                        status(playing ? Playback::playing : Playback::paused, S_OK, std::move(frame));
                    }
                }
            } else if (playing && timestamp + .2 >= position) {
                result = file->pcm(sample, active.audio, seekTo - timestamp);
            }
            sample = {};
            if (FAILED(result)) fail(result);
        }
        setPlaying(active.audio, false);
    }
};

Decoder::Decoder(std::function<void()> notify) : state(std::make_shared<State>(std::move(notify))) {
    std::thread([worker = state] {
        try { worker->run(); }
        catch (...) {
            const auto active = worker->request();
            setPlaying(active.audio, false);
            worker->publish({active.id, Playback::failed, 0, 0, E_FAIL, {}});
        }
    }).detach();
}
Decoder::~Decoder() {
    state->edit([](Request& request) { request.stop = true; setPlaying(request.audio, false); });
}
void Decoder::load(uint32_t id, std::wstring url, double position, std::shared_ptr<Audio> audio) {
    state->edit([&](Request& request) {
        setPlaying(request.audio, false);
        ++request.file;
        request.id = id;
        request.url = std::move(url);
        request.position = position;
        request.audio = std::move(audio);
        request.playing = false;
    });
}
void Decoder::clear() { load(0, {}, 0, {}); }
void Decoder::play(bool enabled) {
    state->edit([&](Request& request) {
        request.playing = enabled;
        if (!enabled) setPlaying(request.audio, false);
    });
}
void Decoder::seek(double position) {
    state->edit([&](Request& request) {
        ++request.seek;
        request.position = position;
        setPlaying(request.audio, false);
    });
}
PlaybackUpdate Decoder::take() {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->posted = false;
    auto update = state->latest;
    state->latest.frame.reset();
    return update;
}

bool verifyHardwareDecode(const std::wstring& url) {
    WindowsMedia media;
    File file;
    auto result = SUCCEEDED(media.status) ? file.open(url) : media.status;
    if (FAILED(result)) return false;
    File::Sample sample;
    for (int count = 0; count < 30; ++count) {
        sample = {};
        if (FAILED(file.read(true, sample)) || sample.end) return false;
        if (sample.payload) {
            VideoFrame frame;
            return SUCCEEDED(file.frame(sample, frame)) && !frame.nv12.empty() && file.sampleRate > 0;
        }
    }
    return false;
}
}



