#include "runtime_paths.hpp"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <string>

namespace viewery::runtime {

std::filesystem::path executableDirectory()
{
    std::wstring filename(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
    if (!length || length == filename.size()) return {};
    filename.resize(length);
    return std::filesystem::path(filename).parent_path();
}

std::filesystem::path userDataDirectory()
{
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &directory))) return {};
    std::filesystem::path result = std::filesystem::path(directory) / L"Viewery";
    CoTaskMemFree(directory);
    return result;
}

std::filesystem::path mediaToolsDirectory()
{
    auto directory = userDataDirectory();
    return directory.empty() ? directory : directory / L"MediaTools";
}

std::filesystem::path mediaToolPath(std::wstring_view filename)
{
    constexpr std::array names{L"libmpv-2.dll", L"mpv-2.dll", L"ffmpeg.exe", L"ffprobe.exe"};
    bool supported = false;
    for (const auto* name : names) supported = supported || filename == name;
    if (!supported) return {};
    for (const auto& directory : {executableDirectory(), mediaToolsDirectory()}) {
        if (directory.empty()) continue;
        const auto candidate = directory / filename;
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
    }
    return {};
}

}
