#include "Api.h"
#include "VideoId.h"
#include <windows.h>
#include <objbase.h>
#include <winhttp.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <limits>
#include "json.hpp"

namespace bilibili {
namespace {
using Microsoft::WRL::ComPtr;
using nlohmann::json;

struct HttpHandle {
    HINTERNET handle;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
};

std::wstring wide(std::string_view input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (!count) return {};
    std::wstring output(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), count);
    return output;
}

std::wstring escape(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr, nullptr);
    std::wstring result;
    for (const unsigned char byte : utf8) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~')
            result.push_back(static_cast<wchar_t>(byte));
        else {
            result += L'%';
            result += L"0123456789ABCDEF"[byte >> 4];
            result += L"0123456789ABCDEF"[byte & 15];
        }
    }
    return result;
}

std::string fetch(const std::wstring& base, const std::wstring& path, size_t maximum = 4 * 1024 * 1024) {
    const auto url = base + path;
    URL_COMPONENTS parsed{};
    parsed.dwStructSize = sizeof parsed;
    parsed.dwHostNameLength = parsed.dwUrlPathLength = parsed.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parsed) ||
        parsed.nScheme != INTERNET_SCHEME_HTTP ||
        std::wstring_view(parsed.lpszHostName, parsed.dwHostNameLength) != L"127.0.0.1") return {};
    auto resource = std::wstring(parsed.lpszUrlPath, parsed.dwUrlPathLength);
    if (parsed.dwExtraInfoLength) resource.append(parsed.lpszExtraInfo, parsed.dwExtraInfoLength);
    const HttpHandle session{WinHttpOpen(L"Audcore Bilibili", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return {};
    WinHttpSetTimeouts(session, 2000, 2500, 8000, 8000);
    const HttpHandle connection{WinHttpConnect(session, L"127.0.0.1", parsed.nPort, 0)};
    if (!connection) return {};
    const HttpHandle request{WinHttpOpenRequest(connection, L"GET", resource.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_ESCAPE_DISABLE)};
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) return {};
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return {};
    std::string body;
    char buffer[16384];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, sizeof buffer, &read) || body.size() + read > maximum) return {};
        if (!read) break;
        body.append(buffer, read);
    }
    return body;
}

json api(const std::wstring& base, const std::wstring& path) {
    const auto data = json::parse(fetch(base, path), nullptr, false);
    if (!data.is_object() || data.value("code", -1) != 0 || !data.contains("data")) return {};
    return data["data"];
}

uint32_t integer(std::wstring_view value, uint32_t fallback, uint32_t maximum) {
    if (value.empty() || value.size() > 7) return fallback;
    uint32_t result = 0;
    for (const auto digit : value) {
        if (digit < L'0' || digit > L'9') return fallback;
        result = result * 10 + static_cast<uint32_t>(digit - L'0');
    }
    return result < fallback || result > maximum ? fallback : result;
}
void replaceAll(std::wstring& value, std::wstring_view before, std::wstring_view after) {
    size_t offset = 0;
    while ((offset = value.find(before, offset)) != value.npos) {
        value.replace(offset, before.size(), after);
        offset += after.size();
    }
}
std::wstring titleText(std::string_view input) {
    auto text = wide(input);
    replaceAll(text, L"<em class=\"keyword\">", L"");
    replaceAll(text, L"</em>", L"");
    replaceAll(text, L"&quot;", L"\"");
    replaceAll(text, L"&#39;", L"'");
    replaceAll(text, L"&lt;", L"<");
    replaceAll(text, L"&gt;", L">");
    replaceAll(text, L"&amp;", L"&");
    return text;
}
}

std::optional<Target> parseTarget(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == text.npos || text.size() > 512) return {};
    text = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
    Target result;
    auto id = text;
    if (const auto scheme = text.find(L"://"); scheme != text.npos) {
        const auto protocol = text.substr(0, scheme);
        if (protocol != L"http" && protocol != L"https") return {};
        text.remove_prefix(scheme + 3);
        const auto slash = text.find(L'/');
        const auto domain = text.substr(0, slash);
        if (slash == text.npos || (domain != L"bilibili.com" && domain != L"www.bilibili.com" && domain != L"m.bilibili.com"))
            return {};
        text.remove_prefix(slash);
        const auto query = text.find(L'?');
        const auto fragment = text.find(L'#');
        id = text.substr(0, std::min(query, fragment));
        if (id.substr(0, 7) != L"/video/") return {};
        id.remove_prefix(7);
        while (!id.empty() && id.back() == L'/') id.remove_suffix(1);
        if (query < fragment) {
            auto data = text.substr(query + 1, fragment == text.npos ? text.npos : fragment - query - 1);
            while (!data.empty()) {
                const auto end = data.find(L'&');
                const auto pair = data.substr(0, end);
                if (pair.substr(0, 2) == L"p=") result.part = static_cast<uint16_t>(integer(pair.substr(2), 1, 9999));
                else if (pair.substr(0, 2) == L"t=") result.seconds = integer(pair.substr(2), 0, 864000);
                if (end == data.npos) break;
                data.remove_prefix(end + 1);
            }
        }
    }
    if (id.size() < 3) return {};
    result.id.assign(id);
    result.id[0] = static_cast<wchar_t>(std::towupper(result.id[0]));
    result.id[1] = static_cast<wchar_t>(std::towupper(result.id[1]));
    return validVideoId(std::wstring_view(result.id)) ? std::optional<Target>(std::move(result)) : std::nullopt;
}

std::optional<SearchPage> search(const std::wstring& base, const std::wstring& query, int page) {
    try {
        const auto data = api(base, L"api/search?keyword=" + escape(query.substr(0, 100)) + L"&page=" + std::to_wstring(page));
        if (!data.is_object()) return {};
        SearchPage result;
        result.pages = data.value("numPages", 0);
        if (!data.contains("result") || !data["result"].is_array()) return result;
        for (const auto& row : data["result"]) {
            if (!row.is_object() || row.value("type", std::string{}) != "video") continue;
            const auto target = parseTarget(wide(row.value("bvid", std::string{})));
            if (!target) continue;
            auto cover = wide(row.value("pic", std::string{}));
            if (cover.substr(0, 2) == L"//") cover.insert(0, L"https:");
            else if (cover.substr(0, 7) == L"http://") cover.insert(4, L"s");
            result.rows.push_back({*target, titleText(row.value("title", std::string{})),
                wide(row.value("author", std::string{})), wide(row.value("duration", std::string{})), std::move(cover), {}});
        }
        return result;
    } catch (...) { return {}; }
}

std::shared_ptr<const Pixels> loadCover(const std::wstring& base, const std::wstring& cover) {
    if (cover.empty()) return {};
    auto bytes = fetch(base, L"api/cover?url=" + escape(cover), 2 * 1024 * 1024);
    if (bytes.empty()) return {};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> image;
    ComPtr<IWICFormatConverter> convert;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &image)) || FAILED(factory->CreateFormatConverter(&convert))) return {};
    UINT width = 0, height = 0;
    if (FAILED(image->GetSize(&width, &height)) || !width || !height || width > 2048 || height > 2048) return {};
    const double factor = std::min({1., 480. / width, 270. / height});
    auto output = std::make_shared<Pixels>();
    output->width = std::max(1u, static_cast<UINT>(width * factor));
    output->height = std::max(1u, static_cast<UINT>(height * factor));
    IWICBitmapSource* source = image.Get();
    if (factor < 1.) {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source, output->width, output->height, WICBitmapInterpolationModeFant))) return {};
        source = scaler.Get();
    }
    if (FAILED(convert->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.,
            WICBitmapPaletteTypeCustom))) return {};
    output->bytes.resize(static_cast<size_t>(output->width) * output->height * 4);
    if (FAILED(convert->CopyPixels(nullptr, output->width * 4, static_cast<UINT>(output->bytes.size()), output->bytes.data()))) return {};
    return output;
}

std::optional<Video> loadVideo(const std::wstring& base, const Target& target) {
    try {
        const auto data = api(base, L"api/view?" + (target.id.substr(0, 2) == L"AV"
            ? L"aid=" + target.id.substr(2) : L"bvid=" + target.id));
        if (!data.is_object() || !data.contains("pages") || !data["pages"].is_array()) return {};
        Video result;
        result.target = target;
        result.target.id = wide(data.value("bvid", std::string{}));
        if (!validVideoId(std::wstring_view(result.target.id))) return {};
        result.title = wide(data.value("title", std::string{}));
        for (const auto& page : data["pages"]) {
            const auto index = page.value("page", 0);
            if (index < 1 || index > 9999) continue;
            result.parts.push_back({static_cast<uint16_t>(index), wide(page.value("part", std::string{}))});
            if (index == target.part) result.cid = page.value("cid", uint64_t{0});
        }
        if (!result.cid) return {};
        const auto play = api(base, L"api/play?bvid=" + result.target.id + L"&cid=" + std::to_wstring(result.cid));
        if (!play.is_object() || !play.contains("durl") || !play["durl"].is_array() || play["durl"].size() != 1) return {};
        const auto url = wide(play["durl"][0].value("url", std::string{}));
        if (url.empty()) return {};
        result.media = base + L"api/media.mp4?url=" + escape(url);
        return result;
    } catch (...) { return {}; }
}

std::vector<Comment> loadComments(const std::wstring& base, uint64_t cid) {
    const auto body = fetch(base, L"api/dm?oid=" + std::to_wstring(cid));
    if (body.empty()) return {};
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()), static_cast<UINT>(body.size())));
    ComPtr<IXmlReader> reader;
    if (!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(stream.Get()))) return {};
    std::vector<Comment> comments;
    XmlNodeType type;
    Comment comment{};
    bool pending = false;
    while (reader->Read(&type) == S_OK && comments.size() < 12000) {
        const wchar_t* value = nullptr;
        UINT count = 0;
        if (type == XmlNodeType_Element && SUCCEEDED(reader->GetLocalName(&value, &count)) &&
            std::wstring_view(value, count) == L"d" && reader->MoveToAttributeByName(L"p", nullptr) == S_OK &&
            SUCCEEDED(reader->GetValue(&value, &count))) {
            int size = 0;
            double time = 0;
            unsigned color = 0;
            int mode = 0;
            if (swscanf_s(value, L"%lf,%d,%d,%u", &time, &mode, &size, &color) == 4 &&
                time >= 0 && time <= 864000 && (mode == 1 || mode == 4 || mode == 5)) {
                comment = {time, mode, color & 0xffffff, {}};
                pending = true;
            }
            reader->MoveToElement();
        } else if (pending && (type == XmlNodeType_Text || type == XmlNodeType_CDATA) &&
            SUCCEEDED(reader->GetValue(&value, &count))) {
            comment.text.assign(value, std::min(count, 160u));
        } else if (pending && type == XmlNodeType_EndElement) {
            if (!comment.text.empty()) comments.push_back(std::move(comment));
            pending = false;
        }
    }
    std::sort(comments.begin(), comments.end(), [](const Comment& a, const Comment& b) { return a.time < b.time; });
    return comments;
}
}
