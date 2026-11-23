#pragma once

#include "frame_image.hpp"
#include "media_types.hpp"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace viewery {

enum class FrameCapturePurpose { clipboard, save };

struct FrameCaptureResult {
    FrameCapturePurpose purpose;
    std::optional<CapturedFrame> frame;
};

struct PlaybackState {
    bool ready = false;
    bool paused = false;
    bool failed = false;
    bool seeking = false;
    double position = 0;
    double duration = 0;
    double speed = 1;
    double volume = 100;
    double zoom = 1;
    double fps = 0;
    int width = 0;
    int height = 0;
    std::wstring videoCodec;
    std::wstring audioCodec;
    std::wstring error;
};

// All methods run on the thread that owns videoHost. A null host uses mpv's
// null outputs, allowing the playback engine to run without an interactive desktop.
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    bool initialize(HWND videoHost);
    const std::wstring& error() const;
    bool open(const std::filesystem::path& path, bool startPaused = false);
    void close();
    void togglePause();
    void stepFrame(int direction);
    void seekRelative(double seconds);
    void seekAbsolute(double seconds);
    void changeSpeed(double factor);
    void changeVolume(double delta);
    void changeZoom(double delta);
    void pan(double dx, double dy);
    void resetView();
    bool requestFrame(FrameCapturePurpose purpose);
    // Call after poll(). A result without pixels reports capture failure.
    std::optional<FrameCaptureResult> takeFrame();
    PlaybackState poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::wstring error_;
};

} // namespace viewery
