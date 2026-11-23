#include "media.hpp"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace viewery {
namespace {

// Subset of the public libmpv client ABI, resolved at runtime so a build of
// Viewery does not require the DLL or import library.
// https://github.com/mpv-player/mpv/blob/v0.40.0/include/mpv/client.h
struct MpvHandle;
enum class Format : int { none = 0, string = 1, flag = 3, integer = 4, real = 5, nodeMap = 8, byteArray = 9 };
enum class EventId : int {
    none = 0, shutdown = 1, commandReply = 5, startFile = 6, endFile = 7,
    seek = 20, playbackRestart = 21, propertyChange = 22, overflow = 24
};
enum class EndReason : int { error = 4, redirect = 5 };

struct NodeList;
struct ByteArray {
    void* data;
    std::size_t size;
};
struct Node {
    union {
        std::int64_t integer;
        NodeList* list;
        char* string;
        ByteArray* bytes;
    } value;
    Format format;
};
struct NodeList {
    int size;
    Node* values;
    char** keys;
};
struct CommandReply { Node result; };
struct StartFile { std::int64_t entryId; };
struct EndFile { EndReason reason; int error; std::int64_t entryId; };
struct Property { const char* name; Format format; void* value; };
struct Event { EventId id; int error; std::uint64_t replyId; void* data; };

static_assert(sizeof(void*) == 8);
static_assert(sizeof(Event) == 24 && sizeof(Node) == 16 && sizeof(Property) == 24);

std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

std::wstring fromUtf8(std::string_view text) {
    if (text.empty() || text.size() > INT_MAX) return {};
    const int size = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), size, result.data(), needed);
    return result;
}

template<class T>
bool loadFunction(HMODULE library, const char* name, T& function) {
    const FARPROC symbol = GetProcAddress(library, name);
    static_assert(sizeof(symbol) == sizeof(function));
    std::memcpy(&function, &symbol, sizeof(function));
    return function != nullptr;
}

struct MpvApi {
    unsigned long (__cdecl* version)() = nullptr;
    MpvHandle* (__cdecl* create)() = nullptr;
    int (__cdecl* initialize)(MpvHandle*) = nullptr;
    void (__cdecl* destroy)(MpvHandle*) = nullptr;
    int (__cdecl* option)(MpvHandle*, const char*, const char*) = nullptr;
    int (__cdecl* command)(MpvHandle*, std::uint64_t, const char**) = nullptr;
    int (__cdecl* observe)(MpvHandle*, std::uint64_t, const char*, Format) = nullptr;
    int (__cdecl* set)(MpvHandle*, std::uint64_t, const char*, Format, void*) = nullptr;
    Event* (__cdecl* waitEvent)(MpvHandle*, double) = nullptr;
    const char* (__cdecl* errorString)(int) = nullptr;
    std::wstring error;

    MpvApi() {
        const auto directory = executableDirectory();
        error = L"Place libmpv 0.40 or later (x64, libmpv-2.dll or mpv-2.dll) next to Viewery.exe.";
        if (directory.empty()) return;
        for (const auto* name : {L"libmpv-2.dll", L"mpv-2.dll"}) {
            const HMODULE library = LoadLibraryExW((directory / name).c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library) continue;

            const bool complete =
                loadFunction(library, "mpv_client_api_version", version) &&
                loadFunction(library, "mpv_create", create) &&
                loadFunction(library, "mpv_initialize", initialize) &&
                loadFunction(library, "mpv_terminate_destroy", destroy) &&
                loadFunction(library, "mpv_set_option_string", option) &&
                loadFunction(library, "mpv_command_async", command) &&
                loadFunction(library, "mpv_observe_property", observe) &&
                loadFunction(library, "mpv_set_property_async", set) &&
                loadFunction(library, "mpv_wait_event", waitEvent) &&
                loadFunction(library, "mpv_error_string", errorString);
            if (complete) {
                const unsigned long api = version();
                if ((api >> 16) == 2 && (api & 0xffffUL) >= 5) {
                    // mpv registers its Win32 window class for the process lifetime.
                    // Keep the DLL mapped for the lifetime of that class.
                    error.clear();
                    return;
                }
            }
            FreeLibrary(library);
            error = L"A compatible build of libmpv 0.40 or later is required (x64, API 2.5+).";
        }
    }
};

constexpr wchar_t inputProperty[] = L"Viewery.MpvVideoInput";

LRESULT CALLBACK videoWindowProcedure(HWND child, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto previous = reinterpret_cast<WNDPROC>(GetPropW(child, inputProperty));
    if (!previous) return DefWindowProcW(child, message, wParam, lParam);
    const HWND root = GetAncestor(child, GA_ROOT);
    switch (message) {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (root && root != child) PostMessageW(root, message, wParam, lParam);
        return 0;
    case WM_RBUTTONDOWN:
        return 0;
    case WM_RBUTTONUP:
        if (root && root != child) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(child, &point);
            PostMessageW(root, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(root), MAKELPARAM(point.x, point.y));
        }
        return 0;
    case WM_CONTEXTMENU:
        if (root && root != child) PostMessageW(root, message, reinterpret_cast<WPARAM>(root), lParam);
        return 0;
    case WM_MOUSEMOVE:
        if ((wParam & MK_LBUTTON) == 0) break;
        [[fallthrough]];
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        if (root && root != child) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // The viewer recognizes clicks across the video window and its host.
            if (message == WM_LBUTTONDBLCLK) message = WM_LBUTTONDOWN;
            MapWindowPoints(child, root, &point, 1);
            PostMessageW(root, message, wParam, MAKELPARAM(point.x, point.y));
        }
        return 0;
    case WM_NCDESTROY:
        RemovePropW(child, inputProperty);
        return CallWindowProcW(previous, child, message, wParam, lParam);
    }
    return CallWindowProcW(previous, child, message, wParam, lParam);
}

void forwardVideoInput(HWND host) {
    // The embedded video window belongs to mpv's GUI thread. It is in our
    // process, so subclass its WNDPROC and post input to our message queue.
    const HWND child = FindWindowExW(host, nullptr, L"mpv", nullptr);
    if (!child || GetPropW(child, inputProperty)) return;
    const auto previous = reinterpret_cast<HANDLE>(GetWindowLongPtrW(child, GWLP_WNDPROC));
    if (!previous || !SetPropW(child, inputProperty, previous)) return;
    if (!SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(videoWindowProcedure))) {
        RemovePropW(child, inputProperty);
    }
}

const Node* nodeField(const Node& node, std::string_view name, Format format) {
    if (node.format != Format::nodeMap || !node.value.list) return nullptr;
    const auto& map = *node.value.list;
    for (int i = 0; i < map.size; ++i) {
        if (map.values[i].format == format && name == map.keys[i]) return &map.values[i];
    }
    return nullptr;
}

std::int64_t resultEntryId(const Event& event) {
    if (!event.data) return 0;
    const auto& node = static_cast<const CommandReply*>(event.data)->result;
    const auto* entry = nodeField(node, "playlist_entry_id", Format::integer);
    return entry ? entry->value.integer : 0;
}

std::optional<CapturedFrame> capturedPixels(const Event& event) {
    if (event.error < 0 || !event.data) return {};
    const auto& node = static_cast<const CommandReply*>(event.data)->result;
    const auto* width = nodeField(node, "w", Format::integer);
    const auto* height = nodeField(node, "h", Format::integer);
    const auto* stride = nodeField(node, "stride", Format::integer);
    const auto* format = nodeField(node, "format", Format::string);
    const auto* data = nodeField(node, "data", Format::byteArray);
    if (!width || !height || !stride || !format || !data || !format->value.string ||
        std::string_view(format->value.string) != "bgra" || !data->value.bytes || !data->value.bytes->data) {
        return {};
    }
    const auto w = width->value.integer;
    const auto h = height->value.integer;
    const auto pitch = stride->value.integer;
    if (w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX) return {};
    const auto rowBytes = static_cast<std::size_t>(w) * 4;
    const auto rows = static_cast<std::size_t>(h);
    constexpr std::size_t maxBytes = 512 * 1024 * 1024;
    if (rows > maxBytes / rowBytes) return {};

    // The BGRA output from the embedded renderer uses a positive pitch.
    // Reject an inconsistent byte array before accessing its rows.
    const auto& bytes = *data->value.bytes;
    if (pitch < static_cast<std::int64_t>(rowBytes) || bytes.size < rowBytes ||
        (rows > 1 && static_cast<std::uint64_t>(pitch) > (bytes.size - rowBytes) / (rows - 1))) {
        return {};
    }
    try {
        CapturedFrame frame{static_cast<int>(w), static_cast<int>(h), {}};
        frame.bgra.resize(rowBytes * rows);
        const auto* pixels = static_cast<const std::uint8_t*>(bytes.data);
        for (std::size_t row = 0; row < rows; ++row) {
            std::memcpy(frame.bgra.data() + row * rowBytes, pixels + row * static_cast<std::size_t>(pitch), rowBytes);
        }
        return frame;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

enum PropertyKey : std::uint64_t {
    paused = 1, seeking, position, duration, width, height, fps, videoCodec, audioCodec, eofReached
};

constexpr struct ObservedProperty {
    PropertyKey key;
    const char* name;
    Format format;
} observedProperties[] = {
    {paused, "pause", Format::flag},
    {seeking, "seeking", Format::flag},
    {position, "time-pos", Format::real},
    {duration, "duration", Format::real},
    {width, "width", Format::integer},
    {height, "height", Format::integer},
    {fps, "container-fps", Format::real},
    {videoCodec, "current-tracks/video/codec", Format::string},
    {audioCodec, "current-tracks/audio/codec", Format::string},
    {eofReached, "eof-reached", Format::flag}
};

double readReal(const Property& property) {
    const double value = property.format == Format::real ? *static_cast<const double*>(property.value) : 0;
    return std::isfinite(value) ? value : 0;
}

int readInteger(const Property& property) {
    if (property.format != Format::integer) return 0;
    const auto value = *static_cast<const std::int64_t*>(property.value);
    return static_cast<int>(std::clamp<std::int64_t>(value, 0, std::numeric_limits<int>::max()));
}

bool readFlag(const Property& property) {
    return property.format == Format::flag && *static_cast<const int*>(property.value) != 0;
}

std::wstring readString(const Property& property) {
    if (property.format != Format::string) return {};
    const auto value = *static_cast<char* const*>(property.value);
    return value ? fromUtf8(value) : std::wstring{};
}

} // namespace

struct MediaPlayer::Impl {
    const MpvApi& api;
    MpvHandle* handle;
    HWND host;
    PlaybackState state;
    std::uint64_t nextRequest = 0;
    std::uint64_t loadRequest = 0;
    std::uint64_t frameRequest = 0;
    FrameCapturePurpose framePurpose = FrameCapturePurpose::clipboard;
    std::optional<FrameCaptureResult> frameResult;
    std::int64_t requestedEntry = 0;
    std::int64_t playingEntry = 0;
    std::optional<MediaKind> kind;
    bool atEnd = false;
    double zoomExponent = 0;
    double panX = 0;
    double panY = 0;

    Impl(const MpvApi& functions, MpvHandle* mpv, HWND window) : api(functions), handle(mpv), host(window) {}
    ~Impl() { api.destroy(handle); }

    template<class... Strings>
    int command(std::uint64_t id, Strings... strings) {
        const char* arguments[] = {strings..., nullptr};
        return api.command(handle, id, arguments);
    }

    bool setNumber(const char* property, double value) {
        return api.set(handle, 0, property, Format::real, &value) >= 0;
    }

    bool hasTimeline() const { return state.ready && kind && *kind != MediaKind::image; }

    void fail(std::wstring message) {
        state.ready = false;
        state.seeking = false;
        state.failed = true;
        state.error = std::move(message);
        if (std::exchange(frameRequest, 0)) {
            frameResult = FrameCaptureResult{framePurpose, std::nullopt};
        }
    }

    void seekTo(double amount, const char* flags) {
        if (!hasTimeline() || !std::isfinite(amount)) return;
        std::array<char, 40> argument{};
        const auto [end, error] = std::to_chars(argument.data(), argument.data() + argument.size() - 1, amount);
        if (error == std::errc{}) command(0, "no-osd", "seek", argument.data(), flags);
    }

    void readProperty(const Event& event) {
        if (!event.data || requestedEntry == 0 || playingEntry != requestedEntry) return;
        const auto& property = *static_cast<const Property*>(event.data);
        switch (event.replyId) {
        case paused: state.paused = kind == MediaKind::image || readFlag(property); break;
        case seeking: state.seeking = readFlag(property); break;
        case position: state.position = readReal(property); break;
        case duration: state.duration = kind == MediaKind::image ? 0 : readReal(property); break;
        case width: state.width = readInteger(property); break;
        case height: state.height = readInteger(property); break;
        case fps: state.fps = kind == MediaKind::image ? 0 : readReal(property); break;
        case videoCodec: state.videoCodec = readString(property); break;
        case audioCodec: state.audioCodec = readString(property); break;
        case eofReached: atEnd = readFlag(property); break;
        }
    }

    void update(const Event& event) {
        switch (event.id) {
        case EventId::commandReply:
            if (loadRequest && loadRequest == event.replyId) {
                requestedEntry = resultEntryId(event);
                if (event.error < 0) {
                    fail(L"Could not open the file (" + fromUtf8(api.errorString(event.error)) + L").");
                } else if (requestedEntry == 0) {
                    fail(L"libmpv did not return a playlist entry for this file.");
                }
            } else if (frameRequest && frameRequest == event.replyId) {
                frameRequest = 0;
                frameResult = FrameCaptureResult{framePurpose, capturedPixels(event)};
            }
            break;
        case EventId::startFile:
            if (event.data) playingEntry = static_cast<const StartFile*>(event.data)->entryId;
            break;
        case EventId::endFile:
            if (const auto* ended = static_cast<const EndFile*>(event.data);
                ended && ended->entryId == requestedEntry) {
                if (ended->reason == EndReason::error) {
                    fail(L"Playback failed (" + fromUtf8(api.errorString(ended->error)) + L").");
                } else if (ended->reason == EndReason::redirect || !state.ready) {
                    fail(L"This file has no playable frames.");
                } else {
                    state.paused = true;
                }
            }
            break;
        case EventId::seek:
            if (requestedEntry != 0 && playingEntry == requestedEntry) state.seeking = true;
            break;
        case EventId::playbackRestart:
            if (requestedEntry != 0 && playingEntry == requestedEntry && !state.failed) {
                state.ready = true;
                state.seeking = false;
            }
            break;
        case EventId::propertyChange: readProperty(event); break;
        case EventId::shutdown: fail(L"The playback engine stopped."); break;
        case EventId::overflow: fail(L"Playback events were lost. Open the file again."); break;
        default: break;
        }
    }
};

MediaPlayer::MediaPlayer() = default;
MediaPlayer::~MediaPlayer() = default;

bool MediaPlayer::initialize(HWND videoHost) {
    if (impl_) return true;
    if (videoHost && GetWindowThreadProcessId(videoHost, nullptr) != GetCurrentThreadId()) {
        error_ = L"The media host must belong to the player thread.";
        return false;
    }
    static const MpvApi api;
    error_ = api.error;
    if (!error_.empty()) return false;

    MpvHandle* handle = api.create();
    if (!handle) {
        error_ = L"Could not create the playback engine.";
        return false;
    }
    auto player = std::make_unique<Impl>(api, handle, videoHost);
    const auto configure = [&apiRef = api, handle, this](const char* option, const char* value) {
        const int result = apiRef.option(handle, option, value);
        if (result >= 0) return true;
        error_ = L"Could not configure libmpv option " + fromUtf8(option) + L" (" +
            fromUtf8(apiRef.errorString(result)) + L").";
        return false;
    };

    constexpr std::pair<const char*, const char*> options[] = {
        {"idle", "yes"}, {"config", "no"}, {"load-scripts", "no"}, {"terminal", "no"},
        {"osc", "no"}, {"osd-level", "0"}, {"input-default-bindings", "no"}, {"input-vo-keyboard", "no"},
        {"drag-and-drop", "no"}, {"autoload-files", "no"}, {"access-references", "no"},
        {"keep-open", "yes"}, {"image-display-duration", "inf"}, {"stop-screensaver", "no"},
        {"hr-seek-framedrop", "no"}, {"framedrop", "no"}, {"volume-max", "100"}
    };
    for (const auto& [option, value] : options) {
        if (!configure(option, value)) return false;
    }

    if (videoHost) {
        const auto id = std::to_string(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(videoHost)));
        if (!configure("wid", id.c_str()) || !configure("vo", "gpu-next,gpu") ||
            !configure("gpu-context", "d3d11") || !configure("hwdec", "auto-safe") ||
            !configure("background-color", "#0d0c12")) return false;
    } else if (!configure("vo", "null") || !configure("ao", "null")) {
        return false;
    }

    if (const int result = api.initialize(handle); result < 0) {
        error_ = L"Could not start libmpv (" + fromUtf8(api.errorString(result)) + L").";
        return false;
    }
    for (const auto& property : observedProperties) {
        if (const int result = api.observe(handle, property.key, property.name, property.format); result < 0) {
            error_ = L"Could not observe playback properties (" + fromUtf8(api.errorString(result)) + L").";
            return false;
        }
    }
    impl_ = std::move(player);
    return true;
}

const std::wstring& MediaPlayer::error() const { return error_; }

bool MediaPlayer::open(const std::filesystem::path& path, bool startPaused) {
    if (!impl_) return false;
    error_.clear();
    const auto kind = classifyMedia(path);
    if (!kind) {
        error_ = L"This file format is unsupported.";
        return false;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec || !std::filesystem::is_regular_file(absolute, ec)) {
        error_ = L"Could not find the file.";
        return false;
    }
    std::u8string filename;
    try {
        filename = absolute.u8string();
    } catch (const std::filesystem::filesystem_error&) {
        error_ = L"Could not read the file path.";
        return false;
    }

    auto& player = *impl_;
    const bool pause = startPaused || *kind == MediaKind::image;
    auto options = std::string(pause ? "pause=yes" : "pause=no") + ",video-zoom=0,video-pan-x=0,video-pan-y=0";
    if (*kind == MediaKind::gif) {
        options += ",loop-file=inf,cache=yes,cache-secs=15,demuxer-max-back-bytes=32MiB";
    }
    const auto request = ++player.nextRequest;
    const int result = player.command(request, "loadfile", reinterpret_cast<const char*>(filename.c_str()),
                                      "replace", "-1", options.c_str());
    if (result < 0) {
        error_ = L"Could not open the file (" + fromUtf8(player.api.errorString(result)) + L").";
        return false;
    }

    player.loadRequest = request;
    player.frameRequest = 0;
    player.frameResult.reset();
    player.requestedEntry = 0;
    player.kind = kind;
    player.atEnd = false;
    player.zoomExponent = 0;
    player.panX = player.panY = 0;
    const double speed = player.state.speed;
    const double volume = player.state.volume;
    player.state = {};
    player.state.paused = pause;
    player.state.speed = speed;
    player.state.volume = volume;
    return true;
}

void MediaPlayer::close() {
    if (!impl_) return;
    auto& player = *impl_;
    player.command(0, "stop");
    player.loadRequest = 0;
    player.frameRequest = 0;
    player.frameResult.reset();
    player.requestedEntry = 0;
    player.kind.reset();
    player.atEnd = false;
    const double speed = player.state.speed;
    const double volume = player.state.volume;
    player.state = {};
    player.state.speed = speed;
    player.state.volume = volume;
}

void MediaPlayer::togglePause() {
    if (!impl_ || !impl_->hasTimeline()) return;
    // With keep-open, a plain unpause at EOF immediately pauses on the last frame again.
    if (impl_->state.paused && impl_->atEnd) {
        if (impl_->command(0, "no-osd", "seek", "0", "absolute+exact") >= 0) {
            impl_->command(0, "no-osd", "set", "pause", "no");
        }
    } else {
        impl_->command(0, "no-osd", "cycle", "pause");
    }
}

void MediaPlayer::stepFrame(int direction) {
    if (!impl_ || !impl_->hasTimeline() || direction == 0) return;
    if (direction < 0) {
        impl_->command(0, "no-osd", "frame-back-step");
    } else {
        impl_->command(0, "no-osd", "frame-step", "1", "mute");
    }
}

void MediaPlayer::seekRelative(double seconds) {
    if (impl_) impl_->seekTo(seconds, "relative+exact");
}

void MediaPlayer::seekAbsolute(double seconds) {
    if (impl_ && std::isfinite(seconds)) impl_->seekTo(std::max(0.0, seconds), "absolute+exact");
}

void MediaPlayer::changeSpeed(double factor) {
    if (!impl_ || !impl_->hasTimeline() || !std::isfinite(factor) || factor <= 0) return;
    const double value = std::clamp(impl_->state.speed * factor, 0.25, 4.0);
    if (impl_->setNumber("speed", value)) impl_->state.speed = value;
}

void MediaPlayer::changeVolume(double delta) {
    if (!impl_ || !impl_->hasTimeline() || !std::isfinite(delta)) return;
    const double value = std::clamp(impl_->state.volume + delta, 0.0, 100.0);
    if (impl_->setNumber("volume", value)) impl_->state.volume = value;
}

void MediaPlayer::changeZoom(double delta) {
    if (!impl_ || !impl_->state.ready || !std::isfinite(delta)) return;
    const double value = std::clamp(impl_->zoomExponent + delta, -2.0, 4.0);
    if (impl_->setNumber("video-zoom", value)) {
        impl_->zoomExponent = value;
        impl_->state.zoom = std::exp2(value);
    }
}

void MediaPlayer::pan(double dx, double dy) {
    if (!impl_ || !impl_->state.ready || !std::isfinite(dx) || !std::isfinite(dy)) return;
    const double x = std::clamp(impl_->panX + dx, -0.75, 0.75);
    const double y = std::clamp(impl_->panY + dy, -0.75, 0.75);
    if (x != impl_->panX && impl_->setNumber("video-pan-x", x)) impl_->panX = x;
    if (y != impl_->panY && impl_->setNumber("video-pan-y", y)) impl_->panY = y;
}

void MediaPlayer::resetView() {
    if (!impl_) return;
    changeZoom(-impl_->zoomExponent);
    pan(-impl_->panX, -impl_->panY);
}

bool MediaPlayer::requestFrame(FrameCapturePurpose purpose) {
    if (!impl_ || !impl_->state.ready || impl_->frameRequest || impl_->frameResult) return false;
    auto& player = *impl_;
    const auto request = ++player.nextRequest;
    if (player.command(request, "screenshot-raw", "video", "bgra") < 0) return false;
    player.framePurpose = purpose;
    player.frameRequest = request;
    return true;
}

std::optional<FrameCaptureResult> MediaPlayer::takeFrame() {
    return impl_ ? std::exchange(impl_->frameResult, std::nullopt) : std::nullopt;
}

PlaybackState MediaPlayer::poll() {
    if (!impl_) return {};
    auto& player = *impl_;
    for (const Event* event = player.api.waitEvent(player.handle, 0); event->id != EventId::none;
         event = player.api.waitEvent(player.handle, 0)) {
        player.update(*event);
    }
    if (player.host && player.state.ready) forwardVideoInput(player.host);
    return player.state;
}

} // namespace viewery
