#include "Text.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>

namespace bilibili {
namespace {
using Microsoft::WRL::ComPtr;

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
}

TextPixels rasterizeText(std::wstring_view text, const TextStyle& style) {
    if (text.empty() || !style.width || !style.height ||
        static_cast<uint64_t>(style.width) * style.height > 4 * 1024 * 1024) return {};
    ComApartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) return {};
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> drawing;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> writing;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteInlineObject> ellipsis;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imaging))) ||
        FAILED(imaging->CreateBitmap(style.width, style.height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing.GetAddressOf())) ||
        FAILED(drawing->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.f, 96.f), &target)) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(style.color & 0xffffff, (style.color >> 24) / 255.f), &brush)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writing.GetAddressOf()))) ||
        FAILED(writing->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, style.size, L"zh-CN", &format))) return {};

    format->SetWordWrapping(style.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (style.centerInk) format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    if (SUCCEEDED(writing->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
        format->SetTrimming(&trimming, ellipsis.Get());
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.f));
    target->DrawText(text.data(), static_cast<UINT32>(text.size()), format.Get(),
        D2D1::RectF(0, 0, static_cast<float>(style.width), static_cast<float>(style.height)), brush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    if (FAILED(target->EndDraw())) return {};

    TextPixels result;
    result.bytes.resize(static_cast<size_t>(style.width) * style.height * 4);
    if (FAILED(bitmap->CopyPixels(nullptr, style.width * 4, static_cast<UINT>(result.bytes.size()), result.bytes.data()))) return {};
    if (style.centerInk) {
        unsigned top = style.height, bottom = 0;
        for (unsigned y = 0; y < style.height; ++y) {
            for (unsigned x = 0; x < style.width; ++x) {
                if (result.bytes[(static_cast<size_t>(y) * style.width + x) * 4 + 3] < 32) continue;
                top = std::min(top, y);
                bottom = y + 1;
                break;
            }
        }
        if (top < bottom) result.offsetY = (static_cast<int>(style.height) - static_cast<int>(top + bottom)) / 2;
    }
    return result;
}
}
