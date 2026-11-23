#include "media.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {

using viewery::MediaKind;
using viewery::MediaPlayer;
using viewery::PlaybackState;
using viewery::FrameCapturePurpose;
using viewery::FrameCaptureResult;

bool expect(bool result, const wchar_t* description) {
    if (!result) std::wcerr << description << L'\n';
    return result;
}

bool approximately(double actual, double expected) {
    return std::abs(actual - expected) < 0.025;
}

template<class Predicate>
bool waitFor(MediaPlayer& player, const wchar_t* description, Predicate condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    PlaybackState state;
    do {
        state = player.poll();
        if (state.failed) break;
        if (condition(state)) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    std::wcerr << description << L" (position=" << state.position << L", paused=" << state.paused
               << L", ready=" << state.ready << L", seeking=" << state.seeking << L", " << state.error << L")\n";
    return false;
}

bool frameAt(MediaPlayer& player, double timestamp, const wchar_t* description) {
    return waitFor(player, description, [timestamp](const PlaybackState& state) {
        return state.ready && state.paused && !state.seeking && approximately(state.position, timestamp);
    });
}

bool openFixture(MediaPlayer& player, const std::filesystem::path& path) {
    if (player.open(path, true)) return true;
    std::wcerr << L"Couldn't open " << path << L" (" << player.error() << L")\n";
    return false;
}

bool checkTimeline(MediaPlayer& player, const std::filesystem::path& path) {
    std::wcout << L"Checking frames in " << path << L'\n';
    if (!openFixture(player, path) || !frameAt(player, 0, L"Initial frame at 0.0 s")) return false;
    if (!waitFor(player, L"Timeline metadata", [](const PlaybackState& state) {
        return state.width == 160 && state.height == 96 && state.duration > 2 && !state.videoCodec.empty();
    })) return false;

    player.stepFrame(1);
    if (!frameAt(player, 0.1, L"Forward frame at 0.1 s")) return false;
    player.stepFrame(1);
    if (!frameAt(player, 0.2, L"Forward frame at 0.2 s")) return false;
    player.stepFrame(1);
    if (!frameAt(player, 0.4, L"Forward frame at 0.4 s")) return false;
    player.stepFrame(-1);
    if (!frameAt(player, 0.2, L"Previous frame is at 0.2 s across a VFR gap")) return false;

    player.seekRelative(0.3);
    if (!frameAt(player, 0.5, L"Relative seek to 0.5 s")) return false;
    player.seekAbsolute(0.8);
    if (!frameAt(player, 0.8, L"Absolute seek to 0.8 s")) return false;

    player.togglePause();
    if (!waitFor(player, L"Playback resumes", [](const PlaybackState& state) { return !state.paused; })) return false;
    player.stepFrame(1);
    return waitFor(player, L"Forward frame step pauses running playback", [](const PlaybackState& state) {
        return state.paused && !state.seeking;
    });
}

bool checkReplay(MediaPlayer& player, const std::filesystem::path& path) {
    if (!player.open(path) || !waitFor(player, L"Video holds its last frame at EOF", [](const PlaybackState& s) {
        return s.ready && s.paused && !s.seeking && s.duration > 2 && s.position >= s.duration - 0.2;
    })) return false;

    player.togglePause();
    return waitFor(player, L"Playback resumes from the beginning after EOF", [](const PlaybackState& s) {
        return s.ready && !s.paused && !s.seeking && s.position > 0.05 && s.position < 1;
    });
}

bool checkCapture(MediaPlayer& player, const std::filesystem::path& path) {
    if (!openFixture(player, path) || !frameAt(player, 0, L"First frame for capture")) return false;
    const auto capture = [&](FrameCapturePurpose purpose) -> std::optional<FrameCaptureResult> {
        if (!player.requestFrame(purpose)) return {};
        std::optional<FrameCaptureResult> result;
        waitFor(player, L"Frame capture completed", [&](const PlaybackState&) {
            result = player.takeFrame();
            return result.has_value();
        });
        return result;
    };
    const auto first = capture(FrameCapturePurpose::save);
    if (!expect(first && first->purpose == FrameCapturePurpose::save && first->frame &&
                first->frame->width == 160 && first->frame->height == 96 &&
                first->frame->bgra.size() == 160 * 96 * 4, L"Original-size BGRA stop frame")) return false;
    player.stepFrame(1);
    if (!frameAt(player, 0.1, L"Next frame for capture")) return false;
    const auto next = capture(FrameCapturePurpose::clipboard);
    return expect(next && next->purpose == FrameCapturePurpose::clipboard && next->frame &&
                  next->frame->bgra != first->frame->bgra, L"Stop frame follows the displayed frame");
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::wcerr << L"Usage: viewery-playback-smoke photo.png animation.gif sample.mp4\n";
        return 2;
    }

    if (!expect(viewery::classifyMedia(L"C:\\media\\Image.PNG") == MediaKind::image, L"Uppercase photo extension") ||
        !expect(viewery::classifyMedia(L"C:\\media\\Animation.Gif") == MediaKind::gif, L"Mixed-case GIF extension") ||
        !expect(viewery::classifyMedia(L"C:\\media\\Movie.MP4") == MediaKind::video, L"Uppercase video extension") ||
        !expect(!viewery::classifyMedia(L"C:\\media\\Document.txt"), L"Unsupported format")) return 1;

    MediaPlayer player;
    if (!player.initialize(nullptr)) {
        std::wcerr << player.error() << L'\n';
        return 1;
    }
    if (!openFixture(player, argv[1]) || !waitFor(player, L"Still image and its dimensions", [](const PlaybackState& s) {
        return s.ready && s.paused && s.width == 160 && s.height == 96 && s.duration == 0;
    })) return 1;
    player.changeZoom(1);
    if (!expect(approximately(player.poll().zoom, 2), L"Still image zoom")) return 1;
    player.resetView();
    if (!expect(approximately(player.poll().zoom, 1), L"Reset photo zoom")) return 1;

    if (!checkTimeline(player, argv[2]) || !checkTimeline(player, argv[3]) ||
        !checkCapture(player, argv[3]) || !checkReplay(player, argv[3])) return 1;
    player.changeSpeed(2);
    player.changeVolume(-25);
    const auto state = player.poll();
    if (!expect(approximately(state.speed, 2) && approximately(state.volume, 75), L"Playback speed and volume")) return 1;
    player.close();
    return expect(!player.poll().ready, L"Close clears the current media") ? 0 : 1;
}
