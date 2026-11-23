#pragma once

#include "explorer.hpp"

#include <functional>
#include <memory>
#include <string>

namespace viewery {

enum class SettingsAction {
    quickHotkey, groupChord, automaticUpdates, renameModule,
    checkUpdates, installUpdate, releaseNotes, mediaTools,
    templates, backupCollection, exportMetadata, importMetadata, repository
};

struct SettingsEvent {
    SettingsAction action;
    // Checkbox: 0/1. Choice: PreviewHotkey value, or 0/1 for the group chord.
    int value = 0;
};

struct SettingsState {
    PreviewHotkey quickHotkey = PreviewHotkey::space;
    bool automaticUpdates = true;
    bool renameEnabled = true;
    bool groupControlChord = false;
    bool updateBusy = false;
    bool canInstall = false;
    std::wstring version;
    std::wstring updateStatus;
};

// UI only: the owner handles persistence, updates and collection actions.
class SettingsWindow {
public:
    using Callback = std::function<void(const SettingsEvent&)>;

    SettingsWindow(HWND owner, Callback callback);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void show(const SettingsState& state, UINT dpi = 0);
    void update(const SettingsState& state);
    void hide();
    bool visible() const;
    HWND window() const;
    // Call before TranslateMessage/DispatchMessage for modeless Tab and Esc.
    bool processMessage(MSG& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace viewery
