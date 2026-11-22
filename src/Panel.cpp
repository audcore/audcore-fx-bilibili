#include "Api.h"
#include "Audio.h"
#include "Decoder.h"
#include "Server.h"
#include "Text.h"
#include "audcore_fx.h"
#include "audcore_fx_editor.h"
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>

namespace {
using namespace bilibili;
using namespace Gdiplus;
using Clock = std::chrono::steady_clock;
constexpr wchar_t windowClass[] = L"Audcore.Bilibili.Panel.2";
constexpr UINT taskReady = WM_APP + 70, videoReady = WM_APP + 71;
constexpr wchar_t demo[] = L"BV1YfJJzVEDF", demoTitle[] = L"《武装乙女》";
constexpr wchar_t version[] = L"v" BILIBILI_VERSION;
constexpr wchar_t purchaseUrl[] = L"https://audcore.utae.cn/bilibili/";

const Color background(0xff181519), surface(0xff241f26), control(0xff312932), hoverColor(0xff433443),
    ink(0xfff5eff4), quiet(0xffc1b1be), accent(0xffff8fb4), onAccent(0xff301522),
    frameColor(0xff483847), toolbar(0xbf000000), dark(0xff000000),
    playerInk(0xfff5f5f5), playerButton(0xff262626), playerHover(0xff3a3a3a), playerSelected(0xff515151);
constexpr float thumbRadius = 4.f;
constexpr float toolbarHeight = 50.f;

struct Words {
    const wchar_t *name, *home, *search, *placeholder, *sample, *more, *back, *danmaku, *parts,
        *play, *pause, *loading, *empty, *searchError, *videoError, *hardware, *audio, *seek;
};
constexpr Words chinese{L"哔哩哔哩", L"返回首页", L"搜索", L"关键词、BV / AV 号或视频链接", L"音乐现场",
    L"加载更多", L"返回", L"弹幕", L"选集", L"播放", L"暂停", L"正在加载…", L"未找到相关视频",
    L"搜索失败，请重试", L"这个视频暂时无法播放", L"当前显卡无法硬解此视频", L"请先启动声核音频", L"播放进度"};
constexpr Words english{L"bilibili", L"Home", L"Search", L"Keywords, BV / AV, or video URL", L"live music",
    L"More", L"Back", L"Danmaku", L"Parts", L"Play", L"Pause", L"Loading…", L"No videos found",
    L"Search failed. Try again.", L"This video is unavailable", L"Hardware decoding unavailable", L"Start Audcore audio first", L"Position"};
constexpr Words japanese{L"ビリビリ", L"ホーム", L"検索", L"キーワード、BV / AV、動画URL", L"音楽ライブ",
    L"もっと見る", L"戻る", L"弾幕", L"話数", L"再生", L"一時停止", L"読込中…", L"動画が見つかりません",
    L"検索に失敗しました", L"再生できない動画です", L"ハードウェア復号不可", L"声核の音声を開始してください", L"再生位置"};

enum class Page { home, results, player };
enum class Action { home, purchase, search, sample, demo, card, more, back, danmaku, parts, play };
enum class ReplyKind { search, cover, video, comments };

struct Job {
    uint32_t generation = 0;
    std::wstring query;
    Target target;
    int page = 1;
    size_t first = 0;
};
struct Reply {
    uint32_t generation;
    ReplyKind kind;
    size_t index = 0;
    std::optional<SearchPage> search;
    std::shared_ptr<const Pixels> image;
    std::optional<Video> video;
    std::vector<Comment> comments;

    Reply(uint32_t generation, ReplyKind kind) : generation(generation), kind(kind) {}
};

class Requests : public std::enable_shared_from_this<Requests> {
    HWND window;
    WPARAM cookie;
    std::wstring base;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Job> job;
    std::deque<Reply> replies;
    uint32_t generation = 0;
    bool stopped = false;

    bool active(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        return !stopped && generation == id;
    }
    void post(Reply value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped || value.generation != generation) return;
        const bool notify = replies.empty();
        replies.push_back(std::move(value));
        if (notify) PostMessageW(window, taskReady, cookie, 0);
    }
    void run() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Job work;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopped || job.has_value(); });
                if (stopped) break;
                work = std::move(*job);
                job.reset();
            }
            try {
                if (work.target.id.empty()) {
                    Reply reply(work.generation, ReplyKind::search);
                    reply.search = search(base, work.query, work.page);
                    std::vector<std::wstring> covers;
                    if (reply.search) for (const auto& row : reply.search->rows) covers.push_back(row.cover);
                    post(std::move(reply));
                    for (size_t index = 0; index < covers.size() && active(work.generation); ++index) {
                        Reply cover(work.generation, ReplyKind::cover);
                        cover.index = work.first + index;
                        cover.image = loadCover(base, covers[index]);
                        if (cover.image) post(std::move(cover));
                    }
                } else {
                    Reply reply(work.generation, ReplyKind::video);
                    reply.video = loadVideo(base, work.target);
                    const auto cid = reply.video ? reply.video->cid : 0;
                    post(std::move(reply));
                    if (cid && active(work.generation)) {
                        Reply dm(work.generation, ReplyKind::comments);
                        dm.comments = loadComments(base, cid);
                        post(std::move(dm));
                    }
                }
            } catch (...) {
                post(Reply(work.generation, work.target.id.empty() ? ReplyKind::search : ReplyKind::video));
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

public:
    Requests(HWND window, WPARAM cookie, std::wstring base) : window(window), cookie(cookie), base(std::move(base)) {}
    void start() { std::thread([self = shared_from_this()] { self->run(); }).detach(); }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopped = true; replies.clear(); }
        ready.notify_one();
    }
    void reset(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        generation = id;
        job.reset();
        replies.clear();
    }
    void submit(Job input) {
        { std::lock_guard<std::mutex> lock(mutex); generation = input.generation; job = std::move(input); }
        ready.notify_one();
    }
    std::deque<Reply> take() {
        std::lock_guard<std::mutex> lock(mutex);
        std::deque<Reply> output;
        output.swap(replies);
        return output;
    }
};

struct Box {
    float x{}, y{}, w{}, h{};
    RectF rect() const { return {x, y, w, h}; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
void fill(Graphics& g, Box box, Color color, float radius = 0) {
    if (box.w <= 0 || box.h <= 0) return;
    SolidBrush brush(color);
    if (radius <= 0) { g.FillRectangle(&brush, box.rect()); return; }
    const float arc = std::min({2 * radius, box.w, box.h});
    GraphicsPath path;
    path.AddArc(box.x, box.y, arc, arc, 180, 90);
    path.AddArc(box.x + box.w - arc, box.y, arc, arc, 270, 90);
    path.AddArc(box.x + box.w - arc, box.y + box.h - arc, arc, arc, 0, 90);
    path.AddArc(box.x, box.y + box.h - arc, arc, arc, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}
void label(Graphics& g, std::wstring_view value, Box box, float size, Color color, bool bold = false, bool centered = false, bool wrap = false, StringAlignment vertical = StringAlignmentNear) {
    if (value.empty() || box.w < 1 || box.h < 1) return;
    Font font(L"Microsoft YaHei UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    StringFormat layout;
    layout.SetFormatFlags(wrap ? StringFormatFlagsLineLimit : StringFormatFlagsNoWrap);
    layout.SetLineAlignment(centered ? StringAlignmentCenter : vertical);
    layout.SetAlignment(centered ? StringAlignmentCenter : StringAlignmentNear);
    layout.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush brush(color);
    g.DrawString(value.data(), static_cast<INT>(value.size()), &font, box.rect(), &layout, &brush);
}
void drawIcon(Graphics& g, Action action, Box box, Color color, bool playing) {
    const float x = box.x + box.w / 2, y = box.y + box.h / 2;
    SolidBrush brush(color);
    Pen pen(color, 1.8f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    switch (action) {
    case Action::home:
    case Action::play:
        if (action == Action::play && playing) {
            g.FillRectangle(&brush, x - 6, y - 7, 4.f, 14.f);
            g.FillRectangle(&brush, x + 2, y - 7, 4.f, 14.f);
        } else {
            PointF points[]{{x - 6, y - 7}, {x + 6, y}, {x - 6, y + 7}};
            g.FillPolygon(&brush, points, 3);
        }
        break;
    case Action::back:
        g.DrawLine(&pen, x - 7, y, x + 7, y);
        g.DrawLine(&pen, x - 7, y, x - 1, y - 6);
        g.DrawLine(&pen, x - 7, y, x - 1, y + 6);
        break;
    case Action::danmaku:
        g.DrawRectangle(&pen, x - 8, y - 6, 16.f, 12.f);
        g.DrawLine(&pen, x - 5, y - 2, x + 4, y - 2);
        g.DrawLine(&pen, x - 5, y + 2, x + 1, y + 2);
        break;
    case Action::parts:
        for (float line : {-5.f, 0.f, 5.f}) g.DrawLine(&pen, x - 7, y + line, x + 7, y + line);
        break;
    default: break;
    }
}
void image(Graphics& g, const uint8_t* pixels, uint32_t width, uint32_t height, Box box, bool crop) {
    if (!pixels || !width || !height || box.w <= 0 || box.h <= 0) return;
    Bitmap bitmap(static_cast<INT>(width), static_cast<INT>(height), static_cast<INT>(width * 4),
        PixelFormat32bppPARGB, const_cast<BYTE*>(pixels));
    const float xScale = box.w / static_cast<float>(width), yScale = box.h / static_cast<float>(height);
    const float factor = crop ? std::max(xScale, yScale) : std::min(xScale, yScale);
    const float w = static_cast<float>(width) * factor, h = static_cast<float>(height) * factor;
    const auto saved = g.Save();
    g.SetClip(box.rect(), CombineModeIntersect);
    g.DrawImage(&bitmap, RectF(box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h));
    g.Restore(saved);
}
std::wstring timeText(double seconds) {
    const auto value = static_cast<unsigned>(std::clamp(seconds, 0., 864000.));
    return std::to_wstring(value / 60) + L":" + (value % 60 < 10 ? L"0" : L"") + std::to_wstring(value % 60);
}

class Panel {
    struct Button {
        HWND hwnd = nullptr;
        Action action{};
        size_t index = 0;
        Box box;
        std::wstring name;
    };
    struct RunningComment { size_t index; unsigned lane; double time; };
    struct TextEntry {
        std::wstring value;
        TextStyle style;
        TextPixels pixels;
    };

    HWND parent, window = nullptr, edit = nullptr, progress = nullptr;
    AudcoreFxEdit editCallback;
    void* editContext;
    std::shared_ptr<Server> server;
    std::shared_ptr<Requests> requests;
    std::unique_ptr<Decoder> decoder;
    std::shared_ptr<Audio> audio;
    const Words* words = &chinese;
    Page page = Page::home, returnPage = Page::home;
    std::wstring query, title;
    const wchar_t* status = nullptr;
    Target target;
    std::optional<Video> currentVideo;
    std::vector<SearchRow> results;
    std::vector<Comment> comments;
    std::vector<RunningComment> shown;
    std::vector<Button> buttons;
    std::shared_ptr<const VideoFrame> frame;
    std::unique_ptr<Bitmap> buffer;
    std::list<TextEntry> textCache;
    size_t cachedTextBytes = 0;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    ULONG_PTR gdiplus = 0;
    size_t usedButtons = 0, nextComment = 0;
    uint32_t generation = 0;
    float width = 720, height = 540, scale = 1, scroll = 0;
    Box editorBox, seekBox, timeBox;
    float homeNameWidth = 0, versionWidth = 0, timeWidth = 0;
    int resultPage = 0, totalPages = 0;
    bool busy = false, dm = true, hovering = false, keyboard = false, started = false, seeking = false;
    Playback playback = Playback::idle;
    double position = 0, duration = 0;
    uint64_t seekRevision = 0;

public:
    Panel(HWND parentWindow, AudcoreFxEdit callback, void* context) : parent(parentWindow), editCallback(callback), editContext(context) {}
    ~Panel() {
        if (requests) requests->stop();
        decoder.reset();
        if (server) stopServer(*server);
        if (window) DestroyWindow(window);
        if (editFont) DeleteObject(editFont);
        if (editBrush) DeleteObject(editBrush);
        buffer.reset();
        if (gdiplus) GdiplusShutdown(gdiplus);
    }

    bool start(int pixelWidth, int pixelHeight, float initialScale) {
        static const wchar_t moduleAnchor = 0;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, &moduleAnchor, &module))
            return false;
        GdiplusStartupInput startup;
        if (GdiplusStartup(&gdiplus, &startup, nullptr) != Ok) return false;
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = module;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = windowClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        const INITCOMMONCONTROLSEX controls{sizeof controls, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"哔哩哔哩", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, pixelWidth, pixelHeight, parent, nullptr, module, this);
        if (!window) return false;
        edit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, nullptr, module, nullptr);
        progress = CreateWindowExW(WS_EX_TRANSPARENT, TRACKBAR_CLASSW, words->seek,
            WS_CHILD | WS_TABSTOP | TBS_NOTICKS, 0, 0, 0, 0, window, nullptr, module, nullptr);
        if (!edit || !progress) return false;
        SendMessageW(edit, EM_SETLIMITTEXT, 512, 0);
        SendMessageW(progress, TBM_SETRANGEMAX, FALSE, 1000);
        SetWindowSubclass(edit, childProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(progress, childProc, 1, reinterpret_cast<DWORD_PTR>(this));
        editBrush = CreateSolidBrush(RGB(36, 31, 38));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(words->placeholder));
        server = startServer();
        if (!server) return false;
        requests = std::make_shared<Requests>(window, reinterpret_cast<WPARAM>(this), panelUrl(*server));
        requests->start();
        resize(pixelWidth, pixelHeight, initialScale);
        return true;
    }
    void resize(int pixelsWide, int pixelsHigh, float factor) {
        scale = std::isfinite(factor) ? std::clamp(factor, .75f, 4.f) : 1.f;
        width = static_cast<float>(std::max(1, pixelsWide)) / scale;
        height = static_cast<float>(std::max(1, pixelsHigh)) / scale;
        SetWindowPos(window, nullptr, 0, 0, pixelsWide, pixelsHigh, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto font = CreateFontW(-static_cast<int>(16 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Microsoft YaHei UI");
        if (font) {
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            if (editFont) DeleteObject(editFont);
            editFont = font;
        }
        layout();
    }
    void bind(const float* values, size_t count) {
        if (!values || count != 3) return;
        const uint32_t id = (static_cast<uint32_t>(values[0]) << 16) | static_cast<uint32_t>(values[1]);
        if (auto next = audioFor(id); next != audio) {
            audio = std::move(next);
            if (audio && page == Page::home) {
                const auto video = audio->video();
                if (!video.id.empty()) open({std::wstring(video.id.begin(), video.id.end()), video.part, 0});
            }
        }
        startVideo();
    }
    void translate(AudcoreFxTranslate callback, void* context) {
        if (!callback) return;
        const auto* name = callback(context, u8"哔哩哔哩");
        if (!name) return;
        const std::string_view translated(name);
        words = translated == "bilibili" ? &english : translated == u8"ビリビリ" ? &japanese : &chinese;
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(words->placeholder));
        SetWindowTextW(progress, words->seek);
        homeNameWidth = 0;
        layout();
    }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->dispatch(hwnd, message, w, l) : DefWindowProcW(hwnd, message, w, l);
    }
    static LRESULT CALLBACK childProc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR context) {
        auto& panel = *reinterpret_cast<Panel*>(context);
        // The panel presents these controls in its back buffer. Their HWNDs
        // provide input and accessibility, and must never paint over that buffer.
        if (hwnd != panel.edit) {
            switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                BeginPaint(hwnd, &paint);
                EndPaint(hwnd, &paint);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_PRINT:
            case WM_PRINTCLIENT: return 0;
            }
        }
        switch (message) {
        case WM_MOUSEMOVE:
            if (hwnd == panel.progress && panel.seeking && GetCapture() == hwnd)
                panel.previewSeek(GET_X_LPARAM(l));
            panel.pointer(hwnd, message);
            break;
        case WM_MOUSELEAVE: panel.pointer(hwnd, message); break;
        case WM_LBUTTONDOWN:
            panel.keyboard = false;
            if (hwnd == panel.progress) {
                if (panel.duration > 0) {
                    SetFocus(hwnd);
                    SetCapture(hwnd);
                    panel.seeking = true;
                    panel.previewSeek(GET_X_LPARAM(l));
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (hwnd == panel.progress) {
                if (panel.seeking) {
                    panel.previewSeek(GET_X_LPARAM(l));
                    panel.seekFromControl();
                    if (GetCapture() == hwnd) ReleaseCapture();
                    panel.pointer(hwnd, WM_MOUSELEAVE);
                }
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (hwnd == panel.progress) return 0;
            break;
        case WM_CAPTURECHANGED:
            if (hwnd == panel.progress && panel.seeking) {
                panel.seeking = false;
                panel.updateThumb();
                InvalidateRect(panel.window, nullptr, FALSE);
            }
            break;
        case WM_MOUSEWHEEL:
            if (panel.page == Page::results) { panel.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0; }
            break;
        case WM_KEYDOWN:
            panel.keyboard = true;
            if (w == VK_TAB) { panel.focusNext(hwnd); return 0; }
            if (w == VK_RETURN && hwnd == panel.edit) { panel.submitEdit(); return 0; }
            if (w == VK_ESCAPE && panel.page == Page::player) { panel.back(); return 0; }
            break;
        case WM_GETDLGCODE: return DefSubclassProc(hwnd, message, w, l) | DLGC_WANTTAB | (hwnd == panel.edit ? DLGC_WANTALLKEYS : 0);
        case WM_SETFOCUS:
        case WM_KILLFOCUS: InvalidateRect(panel.window, nullptr, FALSE); break;
        }
        return DefSubclassProc(hwnd, message, w, l);
    }
    LRESULT dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(hwnd); return 0;
        case WM_DRAWITEM: return TRUE;
        case WM_NOTIFY: {
            const auto* notice = reinterpret_cast<NMHDR*>(l);
            if (notice->hwndFrom == progress && notice->code == NM_CUSTOMDRAW) return CDRF_SKIPDEFAULT;
            break;
        }
        case WM_CTLCOLORBTN: return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(l) == progress) return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
            break;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), RGB(245, 239, 244));
            SetBkColor(reinterpret_cast<HDC>(w), RGB(36, 31, 38));
            return reinterpret_cast<LRESULT>(editBrush);
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED) {
                const auto source = reinterpret_cast<HWND>(l);
                for (size_t i = 0; i < usedButtons; ++i)
                    if (buttons[i].hwnd == source) { invoke(buttons[i].action, buttons[i].index); return 0; }
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(l) == progress) {
                if (!seeking && LOWORD(w) != TB_ENDTRACK) seekFromControl();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE: pointer(hwnd, message); return 0;
        case WM_MOUSEWHEEL: if (page == Page::results) { wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0; } break;
        case WM_LBUTTONDOWN:
            keyboard = false;
            SetFocus(hwnd);
            if (page == Page::player) togglePlay();
            return 0;
        case WM_KEYDOWN:
            keyboard = true;
            if (w == VK_TAB) { layout(); focusNext(hwnd); return 0; }
            if (page == Page::player && w == VK_SPACE) { togglePlay(); return 0; }
            if (page == Page::player && w == VK_ESCAPE) { back(); return 0; }
            break;
        case taskReady:
            if (requests && w == reinterpret_cast<WPARAM>(this)) updateRequests();
            return 0;
        case videoReady:
            if (decoder && w == reinterpret_cast<WPARAM>(this)) updateVideo(decoder->take());
            return 0;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
    void pointer(HWND issuer, UINT message) {
        if (!window) return;
        if (message == WM_MOUSEMOVE) {
            keyboard = false;
            TRACKMOUSEEVENT tracking{sizeof tracking, TME_LEAVE, issuer, 0};
            TrackMouseEvent(&tracking);
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window, &cursor);
        RECT bounds{};
        GetClientRect(window, &bounds);
        const bool inside = PtInRect(&bounds, cursor) != FALSE;
        if (seeking && GetCapture() == progress) return;
        if (page == Page::player && hovering != inside) {
            hovering = inside;
            if (!inside && !IsChild(window, GetFocus())) keyboard = false;
            layout();
        } else if (inside) InvalidateRect(window, nullptr, FALSE);
    }
    void focusNext(HWND current) {
        std::vector<HWND> order;
        if (IsWindowVisible(edit)) order.push_back(edit);
        for (size_t i = 0; i < usedButtons; ++i) if (IsWindowVisible(buttons[i].hwnd)) order.push_back(buttons[i].hwnd);
        if (IsWindowVisible(progress)) order.push_back(progress);
        if (order.empty()) return;
        const bool prev = GetKeyState(VK_SHIFT) < 0;
        const auto found = std::find(order.begin(), order.end(), current);
        const auto next = found == order.end() ? (prev ? order.size() - 1 : 0)
            : (static_cast<size_t>(found - order.begin()) + (prev ? order.size() - 1 : 1)) % order.size();
        SetFocus(order[next]);
    }
    RECT pixelBox(Box box) const {
        const auto x = static_cast<LONG>(box.x * scale), y = static_cast<LONG>(box.y * scale);
        return {x, y, x + std::max(1L, static_cast<LONG>(box.w * scale)),
            y + std::max(1L, static_cast<LONG>(box.h * scale))};
    }
    void place(HWND hwnd, Box box, bool visible) {
        if (visible) {
            const auto bounds = pixelBox(box);
            SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOZORDER | SWP_NOACTIVATE | (hwnd == edit ? 0 : SWP_NOREDRAW));
            if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNA);
        } else if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
    }
    void add(Action action, Box box, std::wstring_view name, size_t index = 0) {
        if (box.w < 1 || box.h < 1) return;
        if (usedButtons == buttons.size()) {
            const auto handle = CreateWindowExW(WS_EX_TRANSPARENT, L"BUTTON", nullptr, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, nullptr, nullptr, nullptr);
            if (!handle) return;
            SetWindowSubclass(handle, childProc, 1, reinterpret_cast<DWORD_PTR>(this));
            Button button;
            button.hwnd = handle;
            buttons.push_back(std::move(button));
        }
        auto& button = buttons[usedButtons++];
        button.action = action;
        button.index = index;
        button.box = box;
        if (button.name != name) { button.name.assign(name); SetWindowTextW(button.hwnd, button.name.c_str()); }
        place(button.hwnd, box, true);
    }
    float inset() const { return std::clamp(width * .04f, 12.f, 42.f); }
    float startY() const { return page == Page::home ? std::max(44.f, height * .42f - 56.f) : 26.f; }
    float textWidth(std::wstring_view value, float size, bool bold) const {
        Graphics graphics(window);
        Font font(L"Microsoft YaHei UI", size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        StringFormat format;
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        RectF measured;
        graphics.MeasureString(value.data(), static_cast<INT>(value.size()), &font, PointF{}, &format, &measured);
        return std::ceil(measured.Width);
    }
    float homeX() const { return std::max(inset(), (width - 56.f - homeNameWidth) / 2); }
    void measureTime() {
        const auto shape = timeText(duration);
        timeWidth = 0;
        for (wchar_t digit = L'0'; digit <= L'9'; ++digit) {
            auto widest = shape;
            for (auto& c : widest) if (c >= L'0' && c <= L'9') c = digit;
            timeWidth = std::max(timeWidth, textWidth(widest + L" / " + widest, 12, false));
        }
    }
    float seekTravel() const { return seekBox.w - 2 * thumbRadius; }
    bool hasParts() const { return currentVideo && currentVideo->parts.size() > 1; }
    float rightControlsX() const { return width - inset() - (hasParts() ? 80.f : 36.f); }
    float resultY() const { return startY() + 110.f + (status ? 24.f : 0.f); }
    unsigned columns() const { return std::max(1u, static_cast<unsigned>(std::max(0.f, width - 2 * inset() + 12) / 207)); }
    float cardWidth() const { return std::max(4.f, (width - 2 * inset() - 12 * static_cast<float>(columns() - 1)) / static_cast<float>(columns())); }
    float cardHeight() const { return cardWidth() * 9 / 16 + 67; }
    float contentHeight() const { return static_cast<float>((results.size() + columns() - 1) / columns()) * (cardHeight() + 12) + (resultPage < totalPages ? 50.f : 0.f); }
    Box card(size_t index) const {
        return {inset() + static_cast<float>(index % columns()) * (cardWidth() + 12),
            resultY() + static_cast<float>(index / columns()) * (cardHeight() + 12) - scroll, cardWidth(), cardHeight()};
    }
    void layout() {
        if (!edit || !progress) return;
        usedButtons = 0;
        const auto margin = inset();
        if (page == Page::player) {
            place(edit, {}, false);
            if (hovering || keyboard) {
                add(Action::back, {margin, 7, 36, 36}, words->back);
                add(Action::danmaku, {rightControlsX(), 7, 36, 36}, words->danmaku);
                if (hasParts()) add(Action::parts, {width - margin - 36.f, 7, 36, 36}, words->parts);
                add(Action::play, {margin, height - 43, 36, 36}, playback == Playback::playing ? words->pause : words->play);
            }
            if (timeWidth == 0) measureTime();
            timeBox = {width - 12.f - timeWidth, height - toolbarHeight, timeWidth, toolbarHeight};
            seekBox = {margin + 48, height - 42, std::max(9.f, timeBox.x - 7.f - margin - 48.f), 35};
            place(progress, seekBox, hovering || keyboard);
            EnableWindow(progress, duration > 0);
        } else {
            place(progress, {}, false);
            const float y = startY(), logo = page == Page::home ? 42.f : 31.f;
            const float searchY = y + (page == Page::home ? 66.f : 45.f);
            if (page == Page::home && homeNameWidth == 0) homeNameWidth = textWidth(words->name, 32, true);
            add(Action::home, {page == Page::home ? homeX() : margin, y, logo, logo}, words->home);
            add(Action::search, {width - margin - 67, searchY + 7, 60, 38}, words->search);
            editorBox = {margin, searchY, width - 2 * margin, 52};
            place(edit, {margin + 15, searchY + 16, std::max(4.f, width - 2 * margin - 97), 23}, true);
            if (page == Page::home) {
                if (versionWidth == 0) versionWidth = textWidth(version, 13, false) + 20.f;
                add(Action::purchase, {width - margin - versionWidth, 10, versionWidth, 26}, version);
                add(Action::sample, {margin, searchY + 64, 100, 28}, words->sample);
                add(Action::demo, {margin + 108, searchY + 64, 116, 28}, demoTitle);
            } else {
                scroll = std::clamp(scroll, 0.f, std::max(0.f, resultY() + contentHeight() - height + 12));
                for (size_t index = 0; index < results.size(); ++index) {
                    const auto rect = card(index);
                    const float top = std::max(rect.y, resultY()), bottom = std::min(rect.y + rect.h, height);
                    if (bottom > top) add(Action::card, {rect.x, top, rect.w, bottom - top}, results[index].title, index);
                }
                const float moreY = resultY() + contentHeight() - scroll - 43;
                if (resultPage < totalPages && moreY >= resultY() && moreY < height)
                    add(Action::more, {(width - 112) / 2, moreY, 112, 32}, words->more);
            }
        }
        for (size_t i = usedButtons; i < buttons.size(); ++i) place(buttons[i].hwnd, {}, false);
        InvalidateRect(window, nullptr, FALSE);
    }

    void wheel(int delta) { scroll -= static_cast<float>(delta) / 120 * 72; layout(); }
    void submitEdit() {
        wchar_t value[513]{};
        GetWindowTextW(edit, value, static_cast<int>(std::size(value)));
        submit(value);
    }
    void submit(std::wstring value) {
        if (auto video = parseTarget(value)) { open(*video); return; }
        const auto first = value.find_first_not_of(L" \t\r\n");
        if (first == value.npos) return;
        query = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
        SetWindowTextW(edit, query.c_str());
        page = Page::results;
        scroll = 0;
        results.clear();
        resultPage = totalPages = 0;
        busy = false;
        requests->reset(++generation);
        more();
    }
    void more() {
        if (busy) return;
        busy = true;
        status = words->loading;
        requests->submit({generation, query, {}, resultPage + 1, results.size()});
        layout();
    }
    void home() {
        page = Page::home;
        query.clear();
        results.clear();
        scroll = 0;
        status = nullptr;
        busy = false;
        requests->reset(++generation);
        SetWindowTextW(edit, L"");
        layout();
        SetFocus(edit);
    }
    void open(const Target& value) {
        if (page != Page::player) returnPage = page;
        if (decoder) decoder->clear();
        requests->reset(++generation);
        page = Page::player;
        target = value;
        title = value.id;
        status = words->loading;
        started = false;
        playback = Playback::loading;
        frame.reset();
        currentVideo.reset();
        comments.clear();
        shown.clear();
        nextComment = 0;
        position = duration = 0;
        seekRevision = 0;
        timeWidth = 0;
        hovering = keyboard = seeking = false;
        SendMessageW(progress, TBM_SETPOS, FALSE, 0);
        requests->submit({generation, {}, target});
        SetFocus(window);
        pointer(window, WM_MOUSELEAVE);
        layout();
    }
    void back() {
        requests->reset(++generation);
        if (decoder) decoder->clear();
        currentVideo.reset();
        frame.reset();
        comments.clear();
        page = returnPage;
        status = nullptr;
        busy = false;
        layout();
        SetFocus(edit);
    }
    void parts() {
        if (!currentVideo || currentVideo->parts.size() < 2) return;
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        for (const auto& part : currentVideo->parts) {
            auto name = L"P" + std::to_wstring(part.page) + L" · " + part.title;
            std::wstring escaped;
            for (const wchar_t c : name) { escaped += c; if (c == L'&') escaped += c; }
            AppendMenuW(menu, MF_STRING | (part.page == target.part ? MF_CHECKED : 0), part.page, escaped.c_str());
        }
        POINT at{static_cast<LONG>((width - inset()) * scale), static_cast<LONG>(46 * scale)};
        ClientToScreen(window, &at);
        const auto part = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, at.x, at.y, window, nullptr);
        DestroyMenu(menu);
        if (part && part != target.part && currentVideo)
            open({currentVideo->target.id, static_cast<uint16_t>(part), 0});
    }
    void invoke(Action action, size_t index) {
        switch (action) {
        case Action::home: home(); break;
        case Action::purchase: ShellExecuteW(window, L"open", purchaseUrl, nullptr, nullptr, SW_SHOWNORMAL); break;
        case Action::search: submitEdit(); break;
        case Action::sample: submit(words->sample); break;
        case Action::demo: open({demo}); break;
        case Action::card: if (index < results.size()) open(results[index].target); break;
        case Action::more: more(); break;
        case Action::back: back(); break;
        case Action::danmaku: dm = !dm; resetComments(position); layout(); break;
        case Action::parts: parts(); break;
        case Action::play: togglePlay(); break;
        }
    }
    void updateRequests() {
        for (auto& result : requests->take()) {
            if (result.generation != generation) continue;
            switch (result.kind) {
            case ReplyKind::search:
                busy = false;
                if (result.search) {
                    ++resultPage;
                    totalPages = result.search->pages;
                    for (auto& row : result.search->rows) results.push_back(std::move(row));
                    status = results.empty() ? words->empty : nullptr;
                } else status = words->searchError;
                layout();
                break;
            case ReplyKind::cover:
                if (result.index < results.size()) results[result.index].image = std::move(result.image);
                InvalidateRect(window, nullptr, FALSE);
                break;
            case ReplyKind::video:
                if (!result.video) { status = words->videoError; playback = Playback::failed; }
                else { currentVideo = std::move(result.video); title = currentVideo->title; startVideo(); }
                layout();
                break;
            case ReplyKind::comments:
                comments = std::move(result.comments);
                resetComments(position);
                InvalidateRect(window, nullptr, FALSE);
                break;
            }
        }
    }
    void startVideo() {
        if (!currentVideo || started || page != Page::player) return;
        if (!audio || !audio->rate.load(std::memory_order_acquire)) { status = words->audio; layout(); return; }
        const auto& video = *currentVideo;
        Selection value{{}, video.target.part};
        for (const auto character : video.target.id) value.id.push_back(static_cast<char>(character));
        if (audio->select(std::move(value)) && editCallback) editCallback(editContext, 11, 0, 0);
        if (!decoder) {
            const auto hwnd = window;
            const auto key = reinterpret_cast<WPARAM>(this);
            decoder = std::make_unique<Decoder>([hwnd, key] { PostMessageW(hwnd, videoReady, key, 0); });
        }
        started = true;
        status = words->loading;
        decoder->load(generation, video.media, video.target.seconds, audio);
        decoder->play(true);
    }
    void togglePlay() {
        if (!decoder || !started) { startVideo(); return; }
        decoder->play(playback != Playback::playing);
    }
    void previewSeek(int mouseX) {
        const auto bounds = pixelBox(seekBox);
        const float logical = (static_cast<float>(bounds.left) + static_cast<float>(mouseX)) / scale - seekBox.x;
        const double fraction = std::clamp((logical - thumbRadius) / seekTravel(), 0.f, 1.f);
        SendMessageW(progress, TBM_SETPOS, FALSE, static_cast<LPARAM>(std::lround(fraction * 1000.)));
        InvalidateRect(window, nullptr, FALSE);
    }
    void updateThumb() {
        SendMessageW(progress, TBM_SETPOS, FALSE,
            static_cast<LPARAM>(duration > 0 ? std::lround(std::clamp(position / duration * 1000., 0., 1000.)) : 0));
    }
    void seekFromControl() {
        seeking = false;
        if (!decoder || duration <= 0) return;
        position = static_cast<double>(SendMessageW(progress, TBM_GETPOS, 0, 0)) * duration / 1000.;
        resetComments(position);
        seekRevision = decoder->seek(position);
        InvalidateRect(window, nullptr, FALSE);
    }
    void updateVideo(PlaybackUpdate update) {
        if (page != Page::player || update.id != generation || update.seekRevision != seekRevision) return;
        const auto previous = playback;
        const bool durationChanged = duration != update.duration;
        playback = update.state;
        if (update.position + .1 < position) resetComments(update.position);
        position = update.position;
        duration = update.duration;
        if (durationChanged) timeWidth = 0;
        if (update.frame) frame = std::move(update.frame);
        if (!seeking) updateThumb();
        if (playback == Playback::playing || playback == Playback::paused || playback == Playback::ended) status = nullptr;
        else if (playback == Playback::failed) status = update.error == static_cast<long>(0x887a0004) ? words->hardware : words->videoError;
        if (previous != playback || durationChanged) layout();
        else InvalidateRect(window, nullptr, FALSE);
    }
    void resetComments(double now) {
        shown.clear();
        nextComment = static_cast<size_t>(std::lower_bound(comments.begin(), comments.end(), now,
            [](const Comment& comment, double seconds) { return comment.time < seconds; }) - comments.begin());
    }
    void videoText(Graphics& g, std::wstring_view value, Box box, float size, Color color,
        bool bold = false, bool wrap = false, bool center = false) {
        if (value.empty() || box.w < 1 || box.h < 1) return;
        const TextStyle style{static_cast<unsigned>(std::ceil(box.w * scale)),
            static_cast<unsigned>(std::ceil(box.h * scale)), size * scale, color.GetValue(), bold, wrap, center};
        const auto existing = std::find_if(textCache.begin(), textCache.end(), [&](const auto& entry) {
            return entry.style == style && entry.value == value;
        });
        if (existing != textCache.end()) textCache.splice(textCache.begin(), textCache, existing);
        else {
            auto pixels = rasterizeText(value, style);
            while (!textCache.empty() && (textCache.size() >= 96 ||
                cachedTextBytes + pixels.bytes.size() > 12 * 1024 * 1024)) {
                cachedTextBytes -= textCache.back().pixels.bytes.size();
                textCache.pop_back();
            }
            cachedTextBytes += pixels.bytes.size();
            textCache.push_front({std::wstring(value), style, std::move(pixels)});
        }
        auto& pixels = textCache.front().pixels;
        if (pixels.bytes.empty()) {
            label(g, value, box, size, color, bold, false, wrap, center ? StringAlignmentCenter : StringAlignmentNear);
            return;
        }
        Bitmap bitmap(style.width, style.height, style.width * 4, PixelFormat32bppPARGB, pixels.bytes.data());
        const auto saved = g.Save();
        g.SetClip(box.rect(), CombineModeIntersect);
        g.DrawImage(&bitmap, RectF(box.x, box.y + pixels.offsetY / scale, style.width / scale, style.height / scale));
        g.Restore(saved);
    }
    void paintComments(Graphics& g) {
        if (!dm) return;
        const unsigned lanes = static_cast<unsigned>(std::clamp(height * .45f / 26, 1.f, 10.f));
        shown.erase(std::remove_if(shown.begin(), shown.end(), [&](const auto& item) {
            return position - item.time >= (comments[item.index].mode == 1 ? 7 : 4);
        }), shown.end());
        while (nextComment < comments.size() && comments[nextComment].time <= position) {
            const auto index = nextComment++;
            const auto& comment = comments[index];
            if (position - comment.time > .3) continue;
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (std::any_of(shown.begin(), shown.end(), [&](const auto& item) {
                    return item.lane == lane && comments[item.index].mode == comment.mode &&
                        (comment.mode != 1 || position - item.time <= 2.);
                })) continue;
                shown.push_back({index, lane, position});
                break;
            }
        }
        for (const auto& item : shown) {
            const auto& comment = comments[item.index];
            const auto length = static_cast<float>(comment.text.size()) * 19.f + 14.f;
            const float offset = static_cast<float>(position - item.time) / 7;
            const float x = comment.mode == 1 ? width - offset * (width + length) : (width - length) / 2;
            const float lane = static_cast<float>(item.lane);
            const float y = comment.mode == 4 ? height - 32 - lane * 26 : 6 + lane * 26;
            videoText(g, comment.text, {x + 1, y + 1, length, 26}, 19, dark, true);
            videoText(g, comment.text, {x, y, length, 26}, 19, Color(0xff000000u | comment.color), true);
        }
    }
    void paint(HWND hwnd) {
        PAINTSTRUCT state{};
        const auto dc = BeginPaint(hwnd, &state);
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        if (bounds.right > 0 && bounds.bottom > 0) {
            if (!buffer || buffer->GetWidth() != static_cast<UINT>(bounds.right) || buffer->GetHeight() != static_cast<UINT>(bounds.bottom))
                buffer = std::make_unique<Bitmap>(bounds.right, bounds.bottom, PixelFormat32bppPARGB);
            {
                Graphics g(buffer.get());
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetInterpolationMode(InterpolationModeBilinear);
                g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
                g.ScaleTransform(scale, scale);
                g.Clear(page == Page::player ? dark : background);
                if (page == Page::player) paintPlayer(g);
                else paintSearch(g);
                paintButtons(g);
            }
            Graphics output(dc);
            if (edit && IsWindowVisible(edit)) {
                // The native EDIT owns this rectangle, including its cue banner and IME text.
                RECT input{};
                GetWindowRect(edit, &input);
                MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&input), 2);
                output.ExcludeClip(Rect(input.left, input.top, input.right - input.left, input.bottom - input.top));
            }
            output.DrawImage(buffer.get(), 0, 0, bounds.right, bounds.bottom);
        }
        EndPaint(hwnd, &state);
    }
    void paintSearch(Graphics& g) {
        const float margin = inset();
        const bool first = page == Page::home;
        const float titleX = first ? homeX() + 56 : margin + 43;
        label(g, words->name, {titleX, startY(), width - margin - titleX, first ? 42.f : 31.f},
            first ? 32.f : 22.f, ink, true, false, false, StringAlignmentCenter);
        fill(g, editorBox, frameColor, 14);
        fill(g, {editorBox.x + 1, editorBox.y + 1, editorBox.w - 2, editorBox.h - 2}, surface, 13);
        if (first) return;
        if (status) label(g, status, {margin, resultY() - 24, width - margin * 2, 22}, 13, quiet);
        const auto saved = g.Save();
        g.SetClip(RectF(0, resultY(), width, std::max(0.f, height - resultY())));
        for (size_t index = 0; index < results.size(); ++index) {
            const auto rect = card(index);
            if (rect.y + rect.h <= resultY() || rect.y >= height) continue;
            const auto& row = results[index];
            fill(g, rect, surface, 11);
            const Box cover{rect.x, rect.y, rect.w, rect.w * 9 / 16};
            fill(g, cover, control, 8);
            if (row.image) image(g, row.image->bytes.data(), row.image->width, row.image->height, cover, true);
            if (!row.duration.empty()) {
                const Box stamp{cover.x + cover.w - 58, cover.y + cover.h - 22, 53, 18};
                fill(g, stamp, toolbar, 5);
                label(g, row.duration, stamp, 11, ink, false, true);
            }
            videoText(g, row.title, {rect.x + 10, cover.y + cover.h + 7, rect.w - 20, 38}, 14, ink, true, true);
            videoText(g, row.author, {rect.x + 10, rect.y + rect.h - 21, rect.w - 20, 18}, 12, quiet);
        }
        g.Restore(saved);
    }
    void paintPlayer(Graphics& g) {
        if (frame && !frame->bgra.empty()) image(g, frame->bgra.data(), frame->width, frame->height, {0, 0, width, height}, false);
        paintComments(g);
        if (status) label(g, status, {8, height / 2 - 18, width - 16, 36}, 15, ink, false, true);
        if (!hovering && !keyboard) return;
        const float margin = inset();
        fill(g, {0, 0, width, toolbarHeight}, toolbar);
        fill(g, {0, height - toolbarHeight, width, toolbarHeight}, toolbar);
        const float x = margin + 47.f;
        const float titleWidth = rightControlsX() - 11.f - x;
        if (titleWidth > 0)
            videoText(g, title, {x, 0, titleWidth, toolbarHeight}, 14, playerInk, true, false, true);
        const float percent = std::clamp(static_cast<float>(SendMessageW(progress, TBM_GETPOS, 0, 0)) / 1000.f, 0.f, 1.f);
        const float center = seekBox.y + seekBox.h / 2;
        fill(g, {seekBox.x + thumbRadius, center - 2, seekTravel(), 4}, playerSelected, 2);
        if (percent > 0) fill(g, {seekBox.x + thumbRadius, center - 2, seekTravel() * percent, 4}, playerInk, 2);
        SolidBrush marker(playerInk);
        g.FillEllipse(&marker, seekBox.x + seekTravel() * percent, center - thumbRadius, 2 * thumbRadius, 2 * thumbRadius);
        const auto elapsed = seeking ? duration * percent : position;
        label(g, timeText(elapsed) + L" / " + timeText(duration), timeBox, 12, playerInk, false, false, false, StringAlignmentCenter);
    }
    void paintButtons(Graphics& g) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window, &cursor);
        for (size_t index = 0; index < usedButtons; ++index) {
            const auto& button = buttons[index];
            const bool hot = button.box.contains(static_cast<float>(cursor.x) / scale, static_cast<float>(cursor.y) / scale);
            if (button.action == Action::card) {
                if (hot || GetFocus() == button.hwnd) fill(g, button.box, Color(32, 255, 143, 180), 8);
                continue;
            }
            const bool pink = button.action == Action::home || button.action == Action::search;
            const bool active = button.action == Action::danmaku && dm;
            const bool player = page == Page::player;
            const auto foreground = player ? playerInk : pink ? onAccent : ink;
            const auto bg = player ? (active ? playerSelected : hot ? playerHover : playerButton)
                : pink ? (hot ? Color(0xffffb2cc) : accent) : hot ? hoverColor : control;
            fill(g, button.box, bg, 9);
            if (button.action == Action::home || button.action == Action::back ||
                button.action == Action::danmaku || button.action == Action::parts || button.action == Action::play)
                drawIcon(g, button.action, button.box, foreground, playback == Playback::playing);
            else label(g, button.name, button.box, button.action == Action::search ? 15.f : 13.f, foreground, pink, true);
            if (keyboard && GetFocus() == button.hwnd) {
                Pen ring(player ? playerInk : accent, 1.3f);
                g.DrawRectangle(&ring, button.box.x + 1, button.box.y + 1, button.box.w - 2, button.box.h - 2);
            }
        }
    }
};
}

extern "C" AUDCORE_FX_EXPORT const AudcoreFxEditor* audcore_fx_get_editor(uint32_t abi) {
    static const AudcoreFxEditor api{
        sizeof api, 1, 720, 540,
        [](void* parent, const double*, size_t, int32_t width, int32_t height, float scale, AudcoreFxEdit edit, void* context) -> void* {
            if (!parent) return nullptr;
            try {
                auto panel = std::make_unique<Panel>(static_cast<HWND>(parent), edit, context);
                return panel->start(width, height, scale) ? panel.release() : nullptr;
            } catch (...) { return nullptr; }
        },
        [](void* handle) { delete static_cast<Panel*>(handle); },
        [](void* handle, int32_t width, int32_t height, float scale) { static_cast<Panel*>(handle)->resize(width, height, scale); },
        [](void*, size_t, double) {},
        [](void* handle, const float* values, size_t count) { static_cast<Panel*>(handle)->bind(values, count); },
        [](void* handle, AudcoreFxTranslate callback, void* context) { static_cast<Panel*>(handle)->translate(callback, context); }
    };
    return abi == 1 ? &api : nullptr;
}
extern "C" AUDCORE_FX_EXPORT uint32_t audcore_fx_editor_flags(uint32_t abi) {
    return abi == 1 ? AUDCORE_FX_EDITOR_RESIZABLE : 0;
}
