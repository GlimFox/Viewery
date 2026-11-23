#include "converter.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct TestFiles {
    fs::path directory;
    fs::path input;
    fs::path animation;
    fs::path clip;
    fs::path canceled;

    ~TestFiles() {
        std::error_code ignored;
        for (const auto* file : {&input, &animation, &clip, &canceled}) fs::remove(*file, ignored);
        fs::remove(directory, ignored);
    }
};

bool check(bool condition, std::wstring_view message) {
    if (!condition) std::wcerr << message << L'\n';
    return condition;
}

bool isGif(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    std::array<char, 6> header{};
    stream.read(header.data(), header.size());
    const std::string_view signature(header.data(), header.size());
    return signature == "GIF87a" || signature == "GIF89a";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::wcerr << L"Usage: converter_smoke.exe <video fixture> <test output directory>\n";
        return 2;
    }

    try {
        const fs::path fixture(argv[1]);
        const auto directory = fs::path(argv[2]) / (L"conversion-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!fs::create_directories(directory)) return 1;
        const TestFiles files{directory, directory / (L"\u041f\u0440\u0438\u043c\u0435\u0440 & video" + fixture.extension().wstring()),
            directory / L"\u042d\u043a\u0441\u043f\u043e\u0440\u0442 complete.gif", directory / L"short clip.gif", directory / L"canceled.gif"};
        fs::copy_file(fixture, files.input);

        const auto full = viewery::convertToGif(files.input, files.animation, viewery::GifPreset::balanced);
        if (!check(full.success, full.message) || !check(isGif(files.animation), L"Whole-video GIF is invalid.")) return 1;
        const auto originalSize = fs::file_size(files.animation);

        const auto clip = viewery::convertToGif(files.input, files.clip, viewery::GifPreset::compact,
            {}, viewery::GifRange{250ms, 500ms});
        if (!check(clip.success, clip.message) || !check(isGif(files.clip), L"Fragment GIF is invalid.")) return 1;
        if (!check(fs::file_size(files.clip) < originalSize, L"Expected a smaller GIF for the shorter fragment.")) return 1;

        const auto clobber = viewery::convertToGif(files.input, files.animation, viewery::GifPreset::compact);
        if (!check(!clobber.success && fs::file_size(files.animation) == originalSize,
                   L"An existing output file must be preserved.")) return 1;

        std::stop_source cancellation;
        cancellation.request_stop();
        const auto canceled = viewery::convertToGif(files.input, files.canceled, viewery::GifPreset::quality,
            cancellation.get_token());
        if (!check(!canceled.success && !fs::exists(files.canceled), L"Cancellation must leave no output file.")) return 1;
        return 0;
    } catch (const fs::filesystem_error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
