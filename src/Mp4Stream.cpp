#include <windows.h>
#include <objbase.h>
#include "Mp4Stream.h"
#include <mfapi.h>
#include <winhttp.h>
#include <wrl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace bilibili {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint64_t blockBytes = 512 * 1024;

struct HttpHandle {
    HINTERNET handle = nullptr;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
};

class RangeStream final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IStream> {
    HttpHandle session, connection;
    std::wstring resource;
    std::mutex mutex;
    std::vector<uint8_t> buffer;
    uint64_t length = 0, cursor = 0, bufferedAt = 0;

    HRESULT fetch(uint64_t start) {
        const auto end = length ? std::min(length, start + blockBytes) - 1 : blockBytes - 1;
        const HttpHandle request{WinHttpOpenRequest(connection, L"GET", resource.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_ESCAPE_DISABLE)};
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        const auto headers = L"Range: bytes=" + std::to_wstring(start) + L"-" + std::to_wstring(end) + L"\r\n";
        if (!request || !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect) ||
            !WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) return STG_E_READFAULT;

        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 206) return STG_E_READFAULT;
        wchar_t contentRange[160]{};
        size = sizeof contentRange;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX,
            contentRange, &size, WINHTTP_NO_HEADER_INDEX)) return STG_E_READFAULT;
        uint64_t first = 0, last = 0, total = 0;
        wchar_t extra = 0;
        if (swscanf_s(contentRange, L"bytes %llu-%llu/%llu%c", &first, &last, &total, &extra, 1u) != 3 ||
            first != start || last < first || last > end || last >= total || !total ||
            total > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()) ||
            (length && total != length)) return STG_E_READFAULT;
        length = total;
        buffer.resize(static_cast<size_t>(last - first + 1));
        bufferedAt = first;
        size_t offset = 0;
        while (offset < buffer.size()) {
            DWORD received = 0;
            if (!WinHttpReadData(request, buffer.data() + offset, static_cast<DWORD>(buffer.size() - offset), &received) ||
                !received) {
                buffer.clear();
                return STG_E_READFAULT;
            }
            offset += received;
        }
        return S_OK;
    }

public:
    HRESULT initialize(const std::wstring& url) {
        URL_COMPONENTS parsed{};
        parsed.dwStructSize = sizeof parsed;
        parsed.dwHostNameLength = parsed.dwUrlPathLength = parsed.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parsed) ||
            parsed.nScheme != INTERNET_SCHEME_HTTP ||
            std::wstring_view(parsed.lpszHostName, parsed.dwHostNameLength) != L"127.0.0.1") return E_INVALIDARG;
        resource.assign(parsed.lpszUrlPath, parsed.dwUrlPathLength);
        if (parsed.dwExtraInfoLength) resource.append(parsed.lpszExtraInfo, parsed.dwExtraInfoLength);
        session.handle = WinHttpOpen(L"Audcore Bilibili", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return STG_E_READFAULT;
        WinHttpSetTimeouts(session, 2000, 2000, 4000, 6500);
        connection.handle = WinHttpConnect(session, L"127.0.0.1", parsed.nPort, 0);
        return connection ? fetch(0) : STG_E_READFAULT;
    }
    IFACEMETHODIMP Read(void* destination, ULONG count, ULONG* written) noexcept override {
        if (written) *written = 0;
        if (!destination && count) return STG_E_INVALIDPOINTER;
        try {
            std::lock_guard<std::mutex> lock(mutex);
            auto* output = static_cast<uint8_t*>(destination);
            ULONG done = 0;
            while (done < count && cursor < length) {
                if (buffer.empty() || cursor < bufferedAt || cursor - bufferedAt >= buffer.size()) {
                    const HRESULT result = fetch(cursor / blockBytes * blockBytes);
                    if (FAILED(result)) return result;
                    if (cursor - bufferedAt >= buffer.size()) return STG_E_READFAULT;
                }
                const auto offset = static_cast<size_t>(cursor - bufferedAt);
                const auto take = std::min<ULONG>(count - done, static_cast<ULONG>(buffer.size() - offset));
                std::memcpy(output + done, buffer.data() + offset, take);
                cursor += take;
                done += take;
            }
            if (written) *written = done;
            return done == count ? S_OK : S_FALSE;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    IFACEMETHODIMP Seek(LARGE_INTEGER distance, DWORD from, ULARGE_INTEGER* position) noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t base = 0;
        if (from == STREAM_SEEK_CUR) base = cursor;
        else if (from == STREAM_SEEK_END) base = length;
        else if (from != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        const auto delta = distance.QuadPart;
        const auto movement = delta < 0 ? static_cast<uint64_t>(-(delta + 1)) + 1 : static_cast<uint64_t>(delta);
        if ((delta < 0 && movement > base) ||
            (delta >= 0 && movement > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()) - base)) return STG_E_INVALIDFUNCTION;
        cursor = delta < 0 ? base - movement : base + movement;
        if (position) position->QuadPart = cursor;
        return S_OK;
    }
    IFACEMETHODIMP Stat(STATSTG* stat, DWORD) noexcept override {
        if (!stat) return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(mutex);
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = length;
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    IFACEMETHODIMP Write(const void*, ULONG, ULONG*) noexcept override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP SetSize(ULARGE_INTEGER) noexcept override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP Commit(DWORD) noexcept override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP Revert() noexcept override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
    IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
    IFACEMETHODIMP Clone(IStream**) noexcept override { return E_NOTIMPL; }
};
}

HRESULT openMp4Stream(const std::wstring& url, IMFByteStream** output) {
    auto range = Microsoft::WRL::Make<RangeStream>();
    if (!range) return E_OUTOFMEMORY;
    HRESULT result = range->initialize(url);
    if (FAILED(result)) return result;
    ComPtr<IMFByteStream> stream;
    result = MFCreateMFByteStreamOnStreamEx(range.Get(), &stream);
    if (FAILED(result)) return result;
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(stream.As(&attributes))) {
        attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
        attributes->SetString(MF_BYTESTREAM_ORIGIN_NAME, L"bilibili.mp4");
    }
    *output = stream.Detach();
    return S_OK;
}
}
