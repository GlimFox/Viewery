#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>

namespace viewery {

enum class MediaKind { image, gif, video };

inline std::optional<MediaKind> classifyMedia(const std::filesystem::path& path)
{
    auto extension = path.extension().native();
    for (auto& character : extension) {
        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
    }
    constexpr std::wstring_view pictures[] = {L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".tif", L".tiff"};
    constexpr std::wstring_view videos[] = {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv",
        L".mpg", L".mpeg", L".m2ts", L".mts", L".ts", L".flv", L".ogv", L".3gp"};
    if (extension == L".gif") return MediaKind::gif;
    if (std::ranges::find(pictures, extension) != std::end(pictures)) return MediaKind::image;
    if (std::ranges::find(videos, extension) != std::end(videos)) return MediaKind::video;
    return std::nullopt;
}

}
