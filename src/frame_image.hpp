#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace viewery {

struct CapturedFrame {
    int width = 0;
    int height = 0;
    // Packed rows from top to bottom. Each pixel is blue, green, red, alpha.
    std::vector<std::uint8_t> bgra;
};

bool copyFrameToClipboard(HWND owner, const CapturedFrame& frame);
bool saveFramePng(const std::filesystem::path& destination, const CapturedFrame& frame, bool overwrite = false);

} // namespace viewery
