#include "converter.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <initializer_list>
#include <string_view>
#include <system_error>
#include <utility>

namespace viewery {
namespace {

namespace fs = std::filesystem;

struct Preset {
    int edge;
    int framesPerSecond;
    int colors;
    std::wstring_view dithering;
};

constexpr Preset settings(GifPreset preset) {
    switch (preset) {
        case GifPreset::compact: return {480, 10, 96, L"bayer:bayer_scale=4"};
        case GifPreset::quality: return {1080, 24, 256, L"sierra2_4a"};
        case GifPreset::balanced: return {720, 15, 192, L"bayer:bayer_scale=2"};
    }
    return {720, 15, 192, L"bayer:bayer_scale=2"};
}

struct Handle {
    explicit Handle(HANDLE value) : value(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    HANDLE value;
};

struct ProcessAttributes {
    ~ProcessAttributes() {
        if (list) DeleteProcThreadAttributeList(list);
        if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    }

    bool initialize() {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        buffer = HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!buffer) {
            SetLastError(ERROR_OUTOFMEMORY);
            return false;
        }
        auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer);
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return false;
        list = attributes;
        return true;
    }

    void* buffer{};
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
};

struct ConversionFiles {
    fs::path folder;
    fs::path palette;
    fs::path animation;
    fs::path log;

    ~ConversionFiles() {
        if (folder.empty()) return;
        std::error_code ignored;
        fs::remove(palette, ignored);
        fs::remove(animation, ignored);
        fs::remove(log, ignored);
        fs::remove(folder, ignored);
    }

    bool create(const fs::path& parent) {
        static std::atomic<unsigned> sequence{0};
        const auto name = L".viewery-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"-" +
            std::to_wstring(++sequence);
        auto candidate = parent / name;
        auto palettePath = candidate / L"palette.png";
        auto animationPath = candidate / L"animation.gif";
        auto logPath = candidate / L"ffmpeg.log";
        if (!fs::create_directory(candidate)) return false;
        folder = std::move(candidate);
        palette = std::move(palettePath);
        animation = std::move(animationPath);
        log = std::move(logPath);
        return true;
    }
};

fs::path ffmpegPath() {
    std::wstring executable(32768, L'\0');
    const auto capacity = static_cast<DWORD>(executable.size());
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), capacity);
    if (length == 0 || length >= capacity) return {};
    executable.resize(length);
    return fs::path(executable).parent_path() / L"ffmpeg.exe";
}

// FFmpeg reads the original UTF-16 command line with CommandLineToArgvW.
void appendArgument(std::wstring& command, std::wstring_view argument) {
    if (!command.empty()) command += L' ';
    command += L'"';
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        command.append(backslashes, L'\\');
        if (character == L'"') command.append(backslashes + 1, L'\\');
        backslashes = 0;
        command += character;
    }
    command.append(backslashes * 2, L'\\');
    command += L'"';
}

void appendArguments(std::wstring& command, std::initializer_list<std::wstring_view> arguments) {
    for (auto argument : arguments) appendArgument(command, argument);
}

std::wstring diagnostic(const fs::path& path) {
    std::ifstream log(path, std::ios::binary | std::ios::ate);
    if (!log || log.tellg() <= 0) return {};
    const auto count = (std::min)(static_cast<std::streamoff>(log.tellg()), std::streamoff{2048});
    log.seekg(-count, std::ios::end);
    std::string bytes(static_cast<size_t>(count), '\0');
    log.read(bytes.data(), count);
    bytes.resize(static_cast<size_t>(log.gcount()));
    const int length = static_cast<int>(bytes.size());
    const int characters = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), length, nullptr, 0);
    if (characters == 0) return {};
    std::wstring result(static_cast<size_t>(characters), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), length, result.data(), characters);
    return result;
}

ConversionResult systemError(std::wstring message, DWORD error) {
    return {false, std::move(message) + L" (Windows error " + std::to_wstring(error) + L")."};
}

ConversionResult runFfmpeg(const fs::path& executable, std::wstring command, const fs::path& logPath,
                           std::stop_token cancellation) {
    if (cancellation.stop_requested()) return {false, L"Conversion canceled."};

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle logFile(CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (logFile.value == INVALID_HANDLE_VALUE) return systemError(L"Cannot open FFmpeg log", GetLastError());
    Handle nullDevice(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (nullDevice.value == INVALID_HANDLE_VALUE) return systemError(L"Cannot open NUL", GetLastError());

    ProcessAttributes attributes;
    std::array handles{logFile.value, nullDevice.value};
    if (!attributes.initialize() ||
        !UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(), sizeof(handles), nullptr, nullptr)) {
        return systemError(L"Cannot set up FFmpeg process", GetLastError());
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullDevice.value;
    startup.StartupInfo.hStdOutput = nullDevice.value;
    startup.StartupInfo.hStdError = logFile.value;
    startup.lpAttributeList = attributes.list;

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process)) {
        return systemError(L"Cannot launch FFmpeg", GetLastError());
    }
    Handle thread(process.hThread);
    Handle child(process.hProcess);
    for (;;) {
        const DWORD result = WaitForSingleObject(child.value, 50);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) return systemError(L"Cannot wait for FFmpeg", GetLastError());
        if (cancellation.stop_requested()) {
            if (TerminateProcess(child.value, ERROR_CANCELLED)) {
                WaitForSingleObject(child.value, INFINITE);
            } else {
                const DWORD error = GetLastError();
                if (WaitForSingleObject(child.value, 0) != WAIT_OBJECT_0) {
                    return systemError(L"Cannot stop FFmpeg", error);
                }
            }
            return {false, L"Conversion canceled."};
        }
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(child.value, &exitCode)) return systemError(L"Cannot read FFmpeg status", GetLastError());
    if (exitCode == 0) return {true, {}};
    return {false, L"FFmpeg failed (exit code " + std::to_wstring(exitCode) + L").\n" + diagnostic(logPath)};
}

std::wstring createFilter(Preset options, std::optional<GifRange> range) {
    const auto edge = std::to_wstring(options.edge);
    std::wstring filter = L"[0:V:0]fps=fps=" + std::to_wstring(options.framesPerSecond) +
        L":start_time=0:eof_action=pass,";
    // Limit after rate normalization so the end of a VFR frame cannot extend the requested clip.
    if (range) filter += L"trim=end=" + std::to_wstring(range->duration.count()) + L"ms,";
    filter += L"scale=w=min(iw\\," + edge + L"):h=min(ih\\," + edge +
        L"):force_original_aspect_ratio=decrease:reset_sar=1:flags=lanczos";
    return filter;
}

}  // namespace

ConversionResult convertToGif(const fs::path& input, const fs::path& output, GifPreset preset,
                              std::stop_token cancellation, std::optional<GifRange> range) {
    if (cancellation.stop_requested()) return {false, L"Conversion canceled."};
    if (input.empty() || output.empty()) return {false, L"Choose an input video and an output GIF."};
    if (range && (range->start.count() < 0 || range->duration.count() <= 0)) {
        return {false, L"Choose a nonnegative start time and a positive duration."};
    }

    try {
        const auto source = fs::absolute(input);
        const auto target = fs::absolute(output);
        if (!fs::is_regular_file(source)) return {false, L"The input video could not be found."};
        if (CompareStringOrdinal(target.extension().c_str(), -1, L".gif", -1, TRUE) != CSTR_EQUAL) {
            return {false, L"The output file must have the .gif extension."};
        }
        if (fs::exists(target)) return {false, L"An output file already exists at that path."};
        const auto ffmpeg = ffmpegPath();
        if (ffmpeg.empty() || !fs::is_regular_file(ffmpeg)) {
            return {false, L"Place ffmpeg.exe next to Viewery.exe to enable GIF conversion."};
        }

        // Both passes must read the same source. Allow other readers including the player.
        Handle sourceReader(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (sourceReader.value == INVALID_HANDLE_VALUE) {
            return systemError(L"Cannot open the input video for conversion", GetLastError());
        }

        const Preset options = settings(preset);
        const auto filter = createFilter(options, range);
        ConversionFiles files;
        if (!files.create(target.parent_path())) return {false, L"Cannot create temporary conversion files."};

        std::wstring inputCommand;
        appendArguments(inputCommand, {ffmpeg.native(), L"-hide_banner", L"-nostdin", L"-loglevel", L"error",
            L"-xerror", L"-n", L"-filter_complex_threads", L"2", L"-threads", L"4", L"-protocol_whitelist", L"file"});
        if (range) {
            appendArguments(inputCommand, {L"-ss", std::to_wstring(range->start.count()) + L"ms",
                L"-t", std::to_wstring(range->duration.count()) + L"ms"});
        }
        appendArguments(inputCommand, {L"-i", source.native()});

        auto paletteCommand = inputCommand;
        const auto paletteFilter = filter + L",palettegen=max_colors=" +
            std::to_wstring(options.colors) + L":stats_mode=diff[p]";
        appendArguments(paletteCommand, {L"-filter_complex", paletteFilter, L"-map", L"[p]", L"-frames:v", L"1",
            L"-f", L"image2", L"-update", L"1", files.palette.native()});
        auto paletteResult = runFfmpeg(ffmpeg, std::move(paletteCommand), files.log, cancellation);
        if (!paletteResult.success) return paletteResult;
        if (!fs::is_regular_file(files.palette)) return {false, L"There are no video frames in the selected range."};

        const auto gifFilter = filter + L"[v];[v][1:v:0]paletteuse=dither=" +
            std::wstring(options.dithering) + L":diff_mode=rectangle[g]";
        appendArguments(inputCommand, {L"-i", files.palette.native(), L"-filter_complex", gifFilter,
            L"-map", L"[g]", L"-loop", L"0", L"-f", L"gif", files.animation.native()});
        auto exportResult = runFfmpeg(ffmpeg, std::move(inputCommand), files.log, cancellation);
        if (!exportResult.success) return exportResult;
        if (cancellation.stop_requested()) return {false, L"Conversion canceled."};
        if (fs::file_size(files.animation) == 0) return {false, L"FFmpeg produced an empty GIF."};

        // Both paths are on the same volume. No replace flag means a competing export wins safely.
        if (!MoveFileExW(files.animation.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return systemError(L"Cannot save the GIF", GetLastError());
        }
        return {true, L"GIF saved."};
    } catch (const fs::filesystem_error& error) {
        return systemError(L"Cannot access a conversion file", static_cast<DWORD>(error.code().value()));
    }
}

}  // namespace viewery
