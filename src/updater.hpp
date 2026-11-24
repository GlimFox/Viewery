#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace viewery {

enum class UpdateStatus {
    idle, checking, current, available, installing, mediaInstalling, mediaReady, failed, unsupported, portableReady
};

struct UpdateInfo {
    UpdateStatus status = UpdateStatus::idle;
    std::wstring version;
    std::wstring notes;
    std::wstring message;
    std::wstring releaseUrl;
    bool canInstall = false;
    bool manual = false;
};

class UpdateService {
public:
    UpdateService(HWND owner, UINT message);
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    bool check(bool manual = false);
    // The caller obtains user confirmation and waits for conversion/rename tasks to finish.
    bool install();
    void cancelPreparedUpdate();
    bool installMediaTools();
    bool busy() const;
    UpdateInfo take() const;

    static bool automaticChecks();
    static bool setAutomaticChecks(bool enabled);

private:
    void publish(UpdateInfo result);

    HWND owner_;
    UINT message_;
    mutable std::mutex mutex_;
    UpdateInfo result_;
    std::wstring installerUri_;
    std::atomic_bool busy_ = false;
    std::jthread worker_;
    HANDLE portableHelper_=nullptr;
};

}
