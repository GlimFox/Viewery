#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace viewery {

enum class PreviewHotkey { space, ctrl_space, ctrl_shift_space, disabled };

class ExplorerIntegration {
public:
    using PreviewCallback = std::function<void(const std::filesystem::path&)>;
    using SupportsFile = std::function<bool(const std::filesystem::path&)>;
    using QuickSpaceCallback = std::function<void(std::uint64_t)>;

    ExplorerIntegration(HWND owner, PreviewCallback open_preview, SupportsFile supports_file,
                        PreviewCallback follow_selection = {}, QuickSpaceCallback quick_space = {});
    ~ExplorerIntegration();

    ExplorerIntegration(const ExplorerIntegration&) = delete;
    ExplorerIntegration& operator=(const ExplorerIntegration&) = delete;

    bool start();
    bool handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    PreviewHotkey hotkey() const;
    bool set_hotkey(PreviewHotkey hotkey);
    void set_follow_selection(bool enabled);
    void set_preview_active(bool enabled);
    HWND source_explorer() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace viewery
