#include "Audio.h"
#include "Decoder.h"
#include "VideoId.h"
#include "Server.h"
#include "audcore_fx.h"
#include "audcore_fx_editor.h"
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <cstring>
#include <cwchar>
#include <WebView2.h>
#include <wrl.h>
#include <string_view>

namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

class Panel : public std::enable_shared_from_this<Panel> {
    HWND parent;
    AudcoreFxEdit edit;
    void* editContext;
    UINT updateMessage = RegisterWindowMessageW(L"Audcore.Bilibili.Playback");
    bool subclassed = false;
    std::unique_ptr<bilibili::Decoder> decoder;
    ComPtr<ICoreWebView2Environment12> environmentWithBuffers;
    ComPtr<ICoreWebView2_17> viewWithBuffers;
    ComPtr<ICoreWebView2SharedBuffer> frameBuffer;
    std::shared_ptr<const bilibili::VideoFrame> pendingFrame;
    uint32_t loadId = 0, frameSequence = 0, awaitingFrame = 0;
    UINT64 bufferSize = 0;
    std::string publishedVideo;
    std::shared_ptr<bilibili::Server> server;
    std::shared_ptr<bilibili::Audio> audio;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::wstring url, language = L"zh-CN";
    RECT bounds{};
    EventRegistrationToken messages{}, navigation{}, popups{}, fullscreen{};
    bool closed = false, subscribed = false, ready = false, com = false, full = false;
    LONG_PTR savedStyle = 0;
    WINDOWPLACEMENT placement{};

public:
    Panel(HWND window, AudcoreFxEdit callback, void* context) : parent(window), edit(callback), editContext(context) {}
    ~Panel() { close(); }

    bool start() {
        // WebView2 can call a completion after the editor has been destroyed.
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) return false;
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
        com = true;
        subclassed = updateMessage && SetWindowSubclass(parent, &windowProc, reinterpret_cast<UINT_PTR>(this),
            reinterpret_cast<DWORD_PTR>(this));
        if (!subclassed) return false;
        server = bilibili::startServer();
        if (!server) return false;
        url = bilibili::panelUrl(*server);
        wchar_t local[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
        if (!length || length >= std::size(local)) return false;
        const auto profile = std::wstring(local) + L"\\Audcore\\BilibiliWebView";
        const auto weak = weak_from_this();
        return SUCCEEDED(CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [weak](HRESULT status, ICoreWebView2Environment* environment) -> HRESULT {
                    auto panel = weak.lock();
                    if (!panel || panel->closed || FAILED(status) || !environment) return S_OK;
                    environment->QueryInterface(IID_PPV_ARGS(&panel->environmentWithBuffers));
                    return environment->CreateCoreWebView2Controller(panel->parent,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [weak](HRESULT result, ICoreWebView2Controller* next) -> HRESULT {
                                if (auto active = weak.lock(); active && !active->closed && SUCCEEDED(result) && next)
                                    active->attach(next);
                                else if (next) next->Close();
                                return S_OK;
                            }).Get());
                }).Get()));
    }

    void resize(int width, int height) {
        bounds = {0, 0, std::max(1, width), std::max(1, height)};
        if (controller) controller->put_Bounds(bounds);
    }

    void bind(const float* values, size_t count) {
        if (!values || count != 3) return;
        const auto id = (static_cast<uint32_t>(values[0]) << 16) | static_cast<uint32_t>(values[1]);
        const auto next = bilibili::audioFor(id);
        if (next != audio) {
            if (decoder) decoder->clear();
            publishedVideo.clear();
            audio = next;
        }
        publish();
    }

    void translate(AudcoreFxTranslate callback, void* context) {
        if (!callback) return;
        const auto localized = callback(context, u8"哔哩哔哩");
        if (!localized) return;
        const std::string_view name(localized);
        language = name == "bilibili" ? L"en" : name == u8"ビリビリ" ? L"ja" : L"zh-CN";
        publish();
    }

    void close() {
        if (closed) return;
        closed = true;
        if (subclassed) RemoveWindowSubclass(parent, &windowProc, reinterpret_cast<UINT_PTR>(this));
        decoder.reset();
        frameBuffer.Reset();
        viewWithBuffers.Reset();
        environmentWithBuffers.Reset();
        setFullscreen(false);
        if (subscribed && view) {
            view->remove_WebMessageReceived(messages);
            view->remove_NavigationStarting(navigation);
            view->remove_NewWindowRequested(popups);
            view->remove_ContainsFullScreenElementChanged(fullscreen);
        }
        if (controller) controller->Close();
        view.Reset();
        controller.Reset();
        if (server) bilibili::stopServer(*server);
        server.reset();
        if (com) CoUninitialize();
    }

private:
    static void moduleAnchor() {}

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data) {
        auto& panel = *reinterpret_cast<Panel*>(data);
        if (message == panel.updateMessage && wparam == id && panel.decoder && !panel.closed) {
            panel.pushUpdate(panel.decoder->take());
            return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, windowProc, id);
        return DefSubclassProc(window, message, wparam, lparam);
    }

    void post(const std::wstring& message) {
        if (view && ready) view->PostWebMessageAsJson(message.c_str());
    }

    void publish() {
        if (!view || !ready) return;
        const bool prepared = audio && audio->rate.load(std::memory_order_acquire) != 0;
        const std::wstring message = L"{\"lang\":\"" + language + L"\",\"ready\":" + (prepared ? L"true" : L"false");
        const auto id = audio ? audio->videoId() : std::string{};
        if (id == publishedVideo) post(message + L"}");
        else {
            publishedVideo = id;
            post(message + L",\"selected\":\"" + std::wstring(id.begin(), id.end()) + L"\"}");
        }
    }

    void publishFrame() {
        if (!pendingFrame || awaitingFrame || !environmentWithBuffers || !viewWithBuffers) return;
        const auto frame = std::move(pendingFrame);
        if (!frameBuffer || bufferSize != frame->nv12.size()) {
            frameBuffer.Reset();
            bufferSize = frame->nv12.size();
            if (FAILED(environmentWithBuffers->CreateSharedBuffer(bufferSize, &frameBuffer))) return;
        }
        BYTE* data = nullptr;
        if (FAILED(frameBuffer->get_Buffer(&data)) || !data) return;
        std::memcpy(data, frame->nv12.data(), frame->nv12.size());
        if (++frameSequence == 0) ++frameSequence;
        const auto metadata = L"{\"id\":" + std::to_wstring(loadId) + L",\"seq\":" + std::to_wstring(frameSequence) +
            L",\"w\":" + std::to_wstring(frame->width) + L",\"h\":" + std::to_wstring(frame->height) +
            L",\"ts\":" + std::to_wstring(frame->timestamp) + L",\"matrix\":\"" +
            (frame->bt709 ? L"bt709" : L"smpte170m") + L"\",\"full\":" + (frame->fullRange ? L"true}" : L"false}");
        if (SUCCEEDED(viewWithBuffers->PostSharedBufferToScript(frameBuffer.Get(),
                COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, metadata.c_str()))) awaitingFrame = frameSequence;
    }

    void pushUpdate(bilibili::PlaybackUpdate update) {
        if (loadId != update.id || !loadId) return;
        post(L"{\"id\":" + std::to_wstring(loadId) + L",\"state\":" + std::to_wstring(static_cast<int>(update.state)) +
            L",\"pos\":" + std::to_wstring(static_cast<int64_t>(update.position * 1000)) +
            L",\"duration\":" + std::to_wstring(static_cast<int64_t>(update.duration * 1000)) +
            L",\"code\":" + std::to_wstring(update.error) + L"}");
        if (update.frame) pendingFrame = std::move(update.frame);
        publishFrame();
    }

    void open(std::wstring_view message) {
        const auto first = message.find(':');
        const auto second = first == message.npos ? message.npos : message.find(':', first + 1);
        if (!audio || second == message.npos || message.size() > 6000) return;
        uint32_t id = 0, time = 0;
        if (!number(message.substr(0, first), id) || id == 0 ||
            !number(message.substr(first + 1, second - first - 1), time)) return;
        const auto resource = message.substr(second + 1);
        if (resource.substr(0, 18) != L"api/media.mp4?url=" ||
            !std::all_of(resource.begin(), resource.end(), [](wchar_t c) { return c > 32 && c < 127 && c != '#'; })) return;
        if (!viewWithBuffers || !environmentWithBuffers) {
            loadId = id;
            pushUpdate({id, bilibili::Playback::failed, 0, 0, E_NOINTERFACE, {}});
            return;
        }
        if (!decoder) {
            auto window = parent;
            const auto event = updateMessage;
            const auto cookie = reinterpret_cast<UINT_PTR>(this);
            decoder = std::make_unique<bilibili::Decoder>([window, event, cookie] { PostMessageW(window, event, cookie, 0); });
        }
        loadId = id;
        pendingFrame.reset();
        decoder->load(id, url + std::wstring(resource), time / 1000., audio);
    }

    static bool number(std::wstring_view text, uint32_t& output) {
        if (text.empty() || text.size() > 9) return false;
        output = 0;
        for (const auto c : text) {
            if (c < '0' || c > '9') return false;
            output = output * 10 + static_cast<uint32_t>(c - '0');
        }
        return true;
    }

    bool local(std::wstring_view location) const {
        return location == url || (location.size() > url.size() && location.substr(0, url.size()) == url &&
            (location[url.size()] == L'?' || location[url.size()] == L'#'));
    }

    void receive(ICoreWebView2WebMessageReceivedEventArgs* args) {
        PWSTR source = nullptr;
        if (FAILED(args->get_Source(&source)) || !source) return;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> origin(source, CoTaskMemFree);
        if (!local(source)) return;
        PWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(raw, CoTaskMemFree);
        const std::wstring_view data(raw);
        if (data == L"ready") {
            ready = true;
            publish();
        } else if (data.substr(0, 5) == L"load:") {
            open(data.substr(5));
        } else if (data.substr(0, 7) == L"select:" && audio) {
            const auto id = data.substr(7);
            if (bilibili::validVideoId(id)) {
                publishedVideo.clear();
                for (const auto character : id) publishedVideo.push_back(static_cast<char>(character));
                if (audio->select(publishedVideo) && edit) edit(editContext, 11, 0, 0);
            }
        } else if (data.substr(0, 6) == L"frame:") {
            uint32_t sequence = 0;
            if (number(data.substr(6), sequence) && sequence == awaitingFrame) {
                awaitingFrame = 0;
                publishFrame();
            }
        } else if (decoder) {
            if (data == L"clear") { loadId = 0; pendingFrame.reset(); decoder->clear(); }
            else if (data == L"play") decoder->play(true);
            else if (data == L"pause") decoder->play(false);
            else if (data.substr(0, 5) == L"seek:") {
                uint32_t position = 0;
                if (number(data.substr(5), position)) decoder->seek(position / 1000.);
            }
        }
    }

    void setFullscreen(bool enter) {
        if (enter == full) return;
        const HWND window = GetAncestor(parent, GA_ROOT);
        if (!window) return;
        if (enter) {
            MONITORINFO monitor{};
            monitor.cbSize = sizeof monitor;
            placement.length = sizeof placement;
            if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) ||
                !GetWindowPlacement(window, &placement)) return;
            savedStyle = GetWindowLongPtrW(window, GWL_STYLE);
            full = true;
            SetWindowLongPtrW(window, GWL_STYLE, savedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
            const auto& rect = monitor.rcMonitor;
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else {
            full = false;
            if (!IsWindow(window)) return;
            SetWindowLongPtrW(window, GWL_STYLE, savedStyle);
            SetWindowPlacement(window, &placement);
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        }
    }

    void attach(ICoreWebView2Controller* next) {
        controller = next;
        if (FAILED(controller->get_CoreWebView2(&view)) || !view) return;
        view.As(&viewWithBuffers);
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(view->get_Settings(&settings))) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
        }
        const auto weak = weak_from_this();
        view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (auto panel = weak.lock(); panel && !panel->closed) panel->receive(args);
                return S_OK;
            }).Get(), &messages);
        view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (auto panel = weak.lock()) {
                    PWSTR raw = nullptr;
                    if (SUCCEEDED(args->get_Uri(&raw)) && raw) {
                        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> uri(raw, CoTaskMemFree);
                        if (!panel->local(raw)) args->put_Cancel(TRUE);
                    }
                } else args->put_Cancel(TRUE);
                return S_OK;
            }).Get(), &navigation);
        view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                return args->put_Handled(TRUE);
            }).Get(), &popups);
        view->add_ContainsFullScreenElementChanged(Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
            [weak](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                if (auto panel = weak.lock(); panel && !panel->closed) {
                    BOOL active = FALSE;
                    if (SUCCEEDED(sender->get_ContainsFullScreenElement(&active))) panel->setFullscreen(active != FALSE);
                }
                return S_OK;
            }).Get(), &fullscreen);
        subscribed = true;
        controller->put_Bounds(bounds);
        view->Navigate(url.c_str());
    }
};

using Handle = std::shared_ptr<Panel>;
Panel& panel(void* handle) { return **static_cast<Handle*>(handle); }
}

extern "C" AUDCORE_FX_EXPORT const AudcoreFxEditor* audcore_fx_get_editor(uint32_t abi) {
    static const AudcoreFxEditor api{
        sizeof api, 1, 720, 440,
        [](void* parent, const double*, size_t, int32_t width, int32_t height, float, AudcoreFxEdit edit, void* context) -> void* {
            if (!parent) return nullptr;
            try {
                auto instance = std::make_shared<Panel>(static_cast<HWND>(parent), edit, context);
                instance->resize(width, height);
                return instance->start() ? new Handle(std::move(instance)) : nullptr;
            } catch (...) { return nullptr; }
        },
        [](void* handle) { auto* owned = static_cast<Handle*>(handle); (*owned)->close(); delete owned; },
        [](void* handle, int32_t width, int32_t height, float) { panel(handle).resize(width, height); },
        [](void*, size_t, double) {},
        [](void* handle, const float* values, size_t count) { panel(handle).bind(values, count); },
        [](void* handle, AudcoreFxTranslate callback, void* context) { panel(handle).translate(callback, context); }
    };
    return abi == 1 ? &api : nullptr;
}

extern "C" AUDCORE_FX_EXPORT uint32_t audcore_fx_editor_flags(uint32_t abi) {
    return abi == 1 ? AUDCORE_FX_EDITOR_RESIZABLE : 0;
}


