#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace viewery {

enum class GifPreset { compact, balanced, quality };

struct GifRange {
    std::chrono::milliseconds start;
    std::chrono::milliseconds duration;
};

struct ConversionResult {
    bool success;
    std::wstring message;
};

// Can run on a worker thread. The output is published only after both passes succeed.
ConversionResult convertToGif(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              GifPreset preset,
                              std::stop_token cancellation = {},
                              std::optional<GifRange> range = std::nullopt);

}  // namespace viewery
