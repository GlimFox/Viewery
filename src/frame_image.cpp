#include "frame_image.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <iterator>
#include <limits>

namespace viewery {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

bool validFrame(const CapturedFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    const auto stride = static_cast<std::uint64_t>(frame.width) * 4;
    return stride * static_cast<unsigned>(frame.height) == frame.bgra.size() &&
           frame.bgra.size() <= std::numeric_limits<UINT>::max();
}

struct StagedFile {
    fs::path path;
    ~StagedFile() {
        if (!path.empty()) DeleteFileW(path.c_str());
    }
};

bool encodePng(const fs::path& path, const CapturedFrame& frame) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> encodedFrame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&encodedFrame, nullptr)) ||
        FAILED(encodedFrame->Initialize(nullptr))) {
        return false;
    }
    const auto width = static_cast<UINT>(frame.width);
    const auto height = static_cast<UINT>(frame.height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    // WritePixels' ABI uses BYTE* for a read-only source buffer.
    return SUCCEEDED(encodedFrame->SetSize(width, height)) &&
           SUCCEEDED(encodedFrame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(encodedFrame->WritePixels(height, width * 4, static_cast<UINT>(frame.bgra.size()),
                                               const_cast<BYTE*>(frame.bgra.data()))) &&
           SUCCEEDED(encodedFrame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace

bool copyFrameToClipboard(HWND owner, const CapturedFrame& frame) {
    if (!owner || !validFrame(frame)) return false;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = frame.width;
    header.bV5Height = -frame.height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(frame.bgra.size());
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + frame.bgra.size());
    if (!memory) return false;
    auto* buffer = static_cast<BYTE*>(GlobalLock(memory));
    if (!buffer) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), frame.bgra.data(), frame.bgra.size());
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    const bool accepted = EmptyClipboard() && SetClipboardData(CF_DIBV5, memory);
    CloseClipboard();
    if (!accepted) GlobalFree(memory);
    return accepted;
}

bool saveFramePng(const fs::path& destination, const CapturedFrame& frame, bool overwrite) {
    if (destination.empty() || !validFrame(frame)) return false;
    GUID unique{};
    wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&unique)) || !StringFromGUID2(unique, name, static_cast<int>(std::size(name)))) return false;

    StagedFile staged;
    fs::path temporary = destination.parent_path() / (std::wstring(L".viewery-frame-") + name + L".tmp");
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    staged.path = std::move(temporary);
    if (!encodePng(staged.path, frame)) return false;

    DWORD moveFlags = MOVEFILE_WRITE_THROUGH;
    if (overwrite) moveFlags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(staged.path.c_str(), destination.c_str(), moveFlags)) return false;
    staged.path.clear();
    return true;
}

} // namespace viewery
