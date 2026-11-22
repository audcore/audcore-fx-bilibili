#pragma once
#include <mfidl.h>
#include <string>

namespace bilibili {
HRESULT openMp4Stream(const std::wstring& url, IMFByteStream** output);
}
