#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#include "Assets.h"
#include "Server.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <string_view>
#include <thread>

namespace bilibili {
namespace {
using namespace std::string_view_literals;
constexpr auto baseHeaders = "Connection: close\r\nCache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: credentialless\r\n"
    "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'; "
    "img-src 'self' https://*.hdslb.com; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n"sv;

struct Internet {
    HINTERNET handle;
    ~Internet() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
};

bool sendAll(SOCKET socket, std::string_view data) {
    while (!data.empty()) {
        const int sent = send(socket, data.data(), static_cast<int>(std::min<size_t>(data.size(), 65536)), 0);
        if (sent <= 0) return false;
        data.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
}
void empty(SOCKET socket, const char* status) {
    sendAll(socket, std::string("HTTP/1.1 ") + status + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
}
void asset(SOCKET socket, std::string_view mime, std::string_view bytes) {
    const auto headers = std::string("HTTP/1.1 200 OK\r\n") + std::string(baseHeaders) +
        "Content-Type: " + std::string(mime) + "; charset=utf-8\r\nContent-Length: " +
        std::to_string(bytes.size()) + "\r\n\r\n";
    if (sendAll(socket, headers)) sendAll(socket, bytes);
}
std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == text.npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}
std::string header(std::string_view request, std::string_view key) {
    auto start = request.find("\r\n");
    while (start != request.npos) {
        start += 2;
        const auto end = request.find("\r\n", start);
        if (end == request.npos || end == start) break;
        const auto line = request.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon == key.size() && std::equal(key.begin(), key.end(), line.begin(),
            [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
            return trim(line.substr(colon + 1));
        start = end;
    }
    return {};
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
std::string decode(std::string_view input) {
    std::string result;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size() || hex(input[i + 1]) < 0 || hex(input[i + 2]) < 0) return {};
            c = static_cast<char>(hex(input[i + 1]) * 16 + hex(input[i + 2]));
            i += 2;
        } else if (c == '+') c = ' ';
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127) return {};
        result.push_back(c);
    }
    return result;
}
bool digits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool rangeOk(std::string_view value) {
    if (value.substr(0, 6) != "bytes=" || value.size() > 48) return false;
    value.remove_prefix(6);
    const auto dash = value.find('-');
    return dash != value.npos && digits(value.substr(0, dash)) &&
        (dash + 1 == value.size() || digits(value.substr(dash + 1)));
}
std::string queryHeader(HINTERNET request, DWORD index) {
    std::array<wchar_t, 256> buffer{};
    DWORD bytes = sizeof buffer;
    if (!WinHttpQueryHeaders(request, index, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    std::wstring_view result(buffer.data(), bytes / sizeof(wchar_t));
    if (!std::all_of(result.begin(), result.end(), [](wchar_t c) { return c >= 32 && c < 127; })) return {};
    std::string ascii;
    ascii.reserve(result.size());
    for (const auto character : result) ascii.push_back(static_cast<char>(character));
    return ascii;
}
bool suffix(std::wstring_view host, std::wstring_view ending) {
    return host.size() > ending.size() && host.substr(host.size() - ending.size()) == ending;
}
}

class Server : public std::enable_shared_from_this<Server> {
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<unsigned> inFlight{0};
    Internet session{nullptr};
    bool winsock = false;
    std::string host, prefix;
public:
    ~Server() { stop(); if (winsock) WSACleanup(); }

    bool start() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
        winsock = true;
        const SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET) return false;
        listener.store(fd);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof address;
        if (bind(fd, reinterpret_cast<const sockaddr*>(&address), size) == SOCKET_ERROR ||
            getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR || listen(fd, 8) == SOCKET_ERROR) return false;
        host = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        std::array<UCHAR, 16> key{};
        if (BCryptGenRandom(nullptr, key.data(), static_cast<ULONG>(key.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return false;
        prefix = "/";
        for (const auto byte : key) {
            prefix += "0123456789abcdef"[byte >> 4];
            prefix += "0123456789abcdef"[byte & 15];
        }
        prefix += "/";
        session.handle = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 4000, 4000, 4000, 5000);
        std::thread([self = shared_from_this(), fd] { self->acceptClients(fd); }).detach();
        return true;
    }

    void stop() {
        const auto socket = listener.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET) closesocket(socket);
    }

    std::wstring url() const {
        const auto text = "http://" + host + prefix;
        return {text.begin(), text.end()};
    }

private:
    void acceptClients(SOCKET fd) {
        while (listener.load() == fd) {
            const SOCKET client = accept(fd, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            if (inFlight.fetch_add(1) < 8) {
                try {
                    std::thread([self = shared_from_this(), client] {
                        try { self->serve(client); } catch (...) {}
                        closesocket(client);
                        self->inFlight.fetch_sub(1);
                    }).detach();
                    continue;
                } catch (...) {}
            }
            closesocket(client);
            inFlight.fetch_sub(1);
        }
    }

    void serve(SOCKET client) {
        const DWORD timeout = 4000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        std::string request;
        std::array<char, 4096> block{};
        while (request.find("\r\n\r\n") == request.npos) {
            const int length = recv(client, block.data(), static_cast<int>(block.size()), 0);
            if (length <= 0 || request.size() + static_cast<size_t>(length) > 16384) return;
            request.append(block.data(), static_cast<size_t>(length));
        }
        const auto line = std::string_view(request).substr(0, request.find("\r\n"));
        const auto end = line.find(' ', 4);
        if (line.substr(0, 4) != "GET " || end == line.npos || header(request, "host") != host) {
            empty(client, "404 Not Found");
            return;
        }
        const auto target = line.substr(4, end - 4);
        if (target.substr(0, prefix.size()) != prefix ||
            !std::all_of(target.begin(), target.end(), [](unsigned char c) { return c > 32 && c < 127; })) {
            empty(client, "404 Not Found");
            return;
        }
        const auto relative = target.substr(prefix.size());
        const auto question = relative.find('?');
        const auto path = relative.substr(0, question);
        const auto args = question == relative.npos ? ""sv : relative.substr(question + 1);
        if (path.empty()) asset(client, "text/html", html);
        else if (path == "panel.css") asset(client, "text/css", css);
        else if (path == "panel.mjs") asset(client, "text/javascript", panelJs);
        else if (path == "routes.mjs") asset(client, "text/javascript", routesJs);
        else if (args.empty()) empty(client, "404 Not Found");
        else if (path == "api/media.mp4" && args.substr(0, 4) == "url=") {
            const auto narrow = decode(args.substr(4));
            const std::wstring text(narrow.begin(), narrow.end());
            URL_COMPONENTS url{};
            url.dwStructSize = sizeof url;
            url.dwHostNameLength = url.dwUrlPathLength = url.dwExtraInfoLength = url.dwUserNameLength = url.dwPasswordLength = DWORD(-1);
            if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &url) ||
                url.nScheme != INTERNET_SCHEME_HTTPS || url.nPort != 443 || url.dwUserNameLength || url.dwPasswordLength ||
                !url.dwHostNameLength || !url.dwUrlPathLength) { empty(client, "404 Not Found"); return; }
            const std::wstring domain(url.lpszHostName, url.dwHostNameLength);
            if (!suffix(domain, L".bilivideo.com") && !suffix(domain, L".bilivideo.cn")) {
                empty(client, "404 Not Found");
                return;
            }
            std::wstring resource(url.lpszUrlPath, url.dwUrlPathLength);
            if (url.dwExtraInfoLength) resource.append(url.lpszExtraInfo, url.dwExtraInfoLength);
            relay(client, domain, resource, "video/mp4", header(request, "range"), true);
        } else if (args.size() <= 2048) {
            std::wstring endpoint;
            if (path == "api/search") endpoint = L"/x/web-interface/wbi/search/type?search_type=video&";
            else if (path == "api/view") endpoint = L"/x/web-interface/view?";
            else if (path == "api/play") endpoint = L"/x/player/playurl?qn=16&fnval=0&platform=html5&";
            else if (path == "api/dm") endpoint = L"/x/v1/dm/list.so?";
            else { empty(client, "404 Not Found"); return; }
            endpoint.append(args.begin(), args.end());
            relay(client, L"api.bilibili.com", endpoint,
                path == "api/dm" ? "text/xml; charset=utf-8" : "application/json; charset=utf-8", {}, false);
        } else empty(client, "404 Not Found");
    }

    void relay(SOCKET client, const std::wstring& domain, const std::wstring& resource,
               std::string_view mime, const std::string& range, bool media) const {
        const Internet connection{WinHttpConnect(session, domain.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connection) { empty(client, "502 Bad Gateway"); return; }
        const Internet request{WinHttpOpenRequest(connection, L"GET", resource.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE)};
        if (!request) { empty(client, "502 Bad Gateway"); return; }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
        if (!media) {
            DWORD compression = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &compression, sizeof compression);
        }
        auto headers = std::wstring(L"Referer: https://www.bilibili.com/\r\n");
        if (media && rangeOk(range)) headers += L"Range: " + std::wstring(range.begin(), range.end()) + L"\r\n";
        if (!WinHttpSendRequest(request, headers.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) { empty(client, "502 Bad Gateway"); return; }
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
            (status != 200 && !(media && status == 206))) { empty(client, "502 Bad Gateway"); return; }

        std::string response = status == 206 ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        response += baseHeaders;
        response += "Content-Type: " + std::string(mime) + "\r\n";
        if (media) {
            const auto length = queryHeader(request, WINHTTP_QUERY_CONTENT_LENGTH);
            const auto contentRange = queryHeader(request, WINHTTP_QUERY_CONTENT_RANGE);
            if (digits(length)) response += "Content-Length: " + length + "\r\n";
            if (status == 206 && contentRange.empty()) { empty(client, "502 Bad Gateway"); return; }
            if (!contentRange.empty()) response += "Content-Range: " + contentRange + "\r\n";
            response += "Accept-Ranges: bytes\r\n";
        }
        response += "\r\n";
        if (!sendAll(client, response)) return;

        std::array<char, 32768> bytes{};
        size_t limit = media ? SIZE_MAX : 4 * 1024 * 1024;
        while (listener.load() != INVALID_SOCKET && limit) {
            DWORD read = 0;
            if (!WinHttpReadData(request, bytes.data(), static_cast<DWORD>(std::min(limit, bytes.size())), &read) ||
                !read || !sendAll(client, {bytes.data(), read})) break;
            limit -= read;
        }
    }
};

std::shared_ptr<Server> startServer() {
    auto server = std::make_shared<Server>();
    return server->start() ? server : nullptr;
}
std::wstring panelUrl(const Server& server) { return server.url(); }
void stopServer(Server& server) { server.stop(); }
}
