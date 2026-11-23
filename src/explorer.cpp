#include "explorer.hpp"

#include <oleauto.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace viewery {
namespace {

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr wchar_t settings_key[] = L"Software\\Viewery";
constexpr wchar_t hotkey_value[] = L"PreviewHotkey";
constexpr UINT preview_message = WM_APP + 0x475;
constexpr UINT follow_message = WM_APP + 0x476;
constexpr int hotkey_probe_id = 0x4a57;
constexpr auto inspection_budget = 150ms;
constexpr DWORD follow_interval_ms = 180;

struct FocusTarget {
    HWND frame = nullptr;
    HWND control = nullptr;
    HWND folder_view = nullptr;

    bool operator==(const FocusTarget&) const = default;
};

std::optional<FocusTarget> explorer_focus() {
    const HWND frame = GetForegroundWindow();
    wchar_t name[40]{};
    if (!frame || !GetClassNameW(frame, name, ARRAYSIZE(name))) return {};
    if (std::wstring_view{name} != L"CabinetWClass" && std::wstring_view{name} != L"ExploreWClass") {
        return {};
    }

    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const DWORD thread = GetWindowThreadProcessId(frame, nullptr);
    constexpr DWORD busy = GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE;
    if (!thread || !GetGUIThreadInfo(thread, &info) || info.hwndActive != frame ||
        !info.hwndFocus || info.hwndCaret || (info.flags & busy) ||
        !GetClassNameW(info.hwndFocus, name, ARRAYSIZE(name))) {
        return {};
    }

    const std::wstring_view control{name};
    if (control != L"DirectUIHWND" && control != L"SysListView32" && control != L"SHELLDLL_DefView") {
        return {};
    }
    for (HWND ancestor = info.hwndFocus; ancestor && ancestor != frame;
         ancestor = GetAncestor(ancestor, GA_PARENT)) {
        if (GetClassNameW(ancestor, name, ARRAYSIZE(name)) && std::wstring_view{name} == L"SHELLDLL_DefView") {
            return FocusTarget{frame, info.hwndFocus, ancestor};
        }
    }
    return {};
}

ComPtr<IUIAutomation> create_automation() {
    ComPtr<IUIAutomation2> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.GetAddressOf())))) {
        return {};
    }
    automation->put_ConnectionTimeout(100);
    automation->put_TransactionTimeout(100);
    return automation;
}

bool file_list_has_focus(IUIAutomation& automation, const FocusTarget& target) {
    ComPtr<IUIAutomationElement> focused;
    if (FAILED(automation.GetFocusedElement(focused.GetAddressOf())) || !focused) return false;

    DWORD explorer_pid = 0;
    GetWindowThreadProcessId(target.frame, &explorer_pid);
    int automation_pid = 0;
    CONTROLTYPEID type = 0;
    if (FAILED(focused->get_CurrentProcessId(&automation_pid)) ||
        static_cast<DWORD>(automation_pid) != explorer_pid ||
        FAILED(focused->get_CurrentControlType(&type))) {
        return false;
    }
    return type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId ||
           type == UIA_ListControlTypeId || type == UIA_DataGridControlTypeId;
}

ComPtr<IFolderView2> active_folder_view(ComPtr<IShellWindows>& windows, const FocusTarget& target) {
    if (!windows) {
        CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(windows.GetAddressOf()));
    }
    long count = 0;
    if (!windows || FAILED(windows->get_Count(&count))) {
        // Recreate a disconnected proxy on the next press after Explorer restarts.
        windows.Reset();
        return {};
    }

    for (long i = 0; i < count; ++i) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> item;
        ComPtr<IWebBrowserApp> window;
        SHANDLE_PTR handle = 0;
        if (FAILED(windows->Item(index, item.GetAddressOf())) || !item ||
            FAILED(item.As(&window)) || FAILED(window->get_HWND(&handle)) ||
            GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT) != target.frame) {
            continue;
        }

        ComPtr<IServiceProvider> provider;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        HWND view_window = nullptr;
        if (FAILED(item.As(&provider)) ||
            FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.GetAddressOf()))) ||
            FAILED(browser->QueryActiveShellView(view.GetAddressOf())) || !view ||
            FAILED(view->GetWindow(&view_window)) || !IsWindowVisible(view_window)) {
            continue;
        }

        // Explorer tabs can report the same top-level HWND. Match the actual file view.
        ComPtr<IFolderView2> folder;
        if (view_window == target.folder_view && SUCCEEDED(view.As(&folder))) {
            return folder;
        }
    }
    return {};
}

std::optional<std::filesystem::path> selected_file(IFolderView2& folder) {
    int selected = -1;
    int focused = -1;
    // Ctrl+Arrow can move focus without changing Explorer's selection.
    if (folder.GetFocusedItem(&focused) != S_OK || focused < 0 ||
        folder.GetSelectedItem(0, &selected) != S_OK || focused != selected) {
        return {};
    }

    ComPtr<IShellItemArray> selection;
    DWORD count = 0;
    ComPtr<IShellItem> item;
    if (folder.GetSelection(FALSE, selection.GetAddressOf()) != S_OK || !selection ||
        FAILED(selection->GetCount(&count)) || count != 1 ||
        FAILED(selection->GetItemAt(0, item.GetAddressOf())) || !item) {
        return {};
    }

    SFGAOF attributes = 0;
    constexpr SFGAOF flags = SFGAO_FILESYSTEM | SFGAO_FOLDER;
    if (FAILED(item->GetAttributes(flags, &attributes)) || (attributes & flags) != SFGAO_FILESYSTEM) {
        return {};
    }

    PWSTR file_name = nullptr;
    const HRESULT status = item->GetDisplayName(SIGDN_FILESYSPATH, &file_name);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_name(file_name, CoTaskMemFree);
    if (FAILED(status) || !owned_name) return {};
    return std::filesystem::path(owned_name.get());
}

class FocusInspection {
public:
    explicit FocusInspection(HWND owner) : owner_(owner) {}

    ~FocusInspection() {
        if (activity_) CloseHandle(activity_);
    }

    static std::shared_ptr<FocusInspection> create(HWND owner) {
        auto inspection = std::make_shared<FocusInspection>(owner);
        if (!inspection->activity_) return {};
        std::thread([inspection] { inspection->run(); }).detach();
        return inspection;
    }

    std::optional<std::filesystem::path> request(const FocusTarget& target) {
        std::unique_lock lock(mutex_);
        if (job_) return {};
        job_ = target;
        waiting_ = true;
        SetEvent(activity_);
        condition_.wait_for(lock, inspection_budget, [this] { return !job_; });
        waiting_ = false;
        return std::exchange(result_, std::nullopt);
    }

    void stop() {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        SetEvent(activity_);
    }

    void follow(HWND frame, const std::filesystem::path& initial) {
        std::lock_guard lock(mutex_);
        follow_frame_ = frame;
        last_followed_ = initial;
        follow_result_.reset();
        ++follow_generation_;
        SetEvent(activity_);
    }

    std::optional<std::filesystem::path> take_followed_file() {
        std::lock_guard lock(mutex_);
        auto result = std::exchange(follow_result_, std::nullopt);
        if (!result || explorer_focus() != result->target) return {};
        last_followed_ = result->path;
        return std::move(result->path);
    }

private:
    struct FollowResult {
        FocusTarget target;
        std::filesystem::path path;
    };

    std::optional<std::filesystem::path> lookup(const FocusTarget& target, ComPtr<IUIAutomation>& automation,
                                               ComPtr<IShellWindows>& shells) {
        try {
            if (explorer_focus() != target) return {};
            if (!automation) automation = create_automation();
            if (!automation || !file_list_has_focus(*automation.Get(), target)) return {};
            auto folder = active_folder_view(shells, target);
            if (folder) return selected_file(*folder.Get());
        } catch (...) {
            // Busy or disconnected Explorer providers leave the current preview alone.
        }
        return {};
    }

    void check_followed_file(ComPtr<IUIAutomation>& automation, ComPtr<IShellWindows>& shells) {
        const auto target = explorer_focus();
        unsigned generation = 0;
        {
            std::lock_guard lock(mutex_);
            if (!follow_frame_ || stopping_ || job_) return;
            if (!IsWindow(follow_frame_)) {
                follow_frame_ = nullptr;
                follow_result_.reset();
                return;
            }
            if (!target || target->frame != follow_frame_) return;
            generation = follow_generation_;
        }

        auto path = lookup(*target, automation, shells);
        std::lock_guard lock(mutex_);
        if (stopping_ || job_ || generation != follow_generation_) return;
        if (!path || *path == last_followed_) {
            follow_result_.reset();
            return;
        }
        const bool pending = follow_result_.has_value();
        follow_result_ = FollowResult{*target, std::move(*path)};
        if (!pending && !PostMessageW(owner_, follow_message, 0, 0)) follow_result_.reset();
    }

    void run() {
        // Explorer's IShellView::GetWindow proxy requires a caller that can pump
        // STA messages. From an MTA it can return RPC_E_CANTCALLOUT_ININPUTSYNCCALL.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com)) return;
        {
            auto automation = create_automation();
            ComPtr<IShellWindows> shells;
            CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_ALL, IID_PPV_ARGS(shells.GetAddressOf()));
            for (;;) {
                DWORD timeout;
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_) break;
                    timeout = follow_frame_ ? follow_interval_ms : INFINITE;
                }
                const DWORD event = MsgWaitForMultipleObjectsEx(1, &activity_, timeout, QS_ALLINPUT,
                                                               MWMO_INPUTAVAILABLE);
                if (event == WAIT_TIMEOUT) {
                    check_followed_file(automation, shells);
                    continue;
                }
                if (event == WAIT_OBJECT_0 + 1) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        DispatchMessageW(&message);
                    }
                    continue;
                }
                if (event != WAIT_OBJECT_0) break;
                FocusTarget target;
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_) break;
                    if (!job_) continue;
                    target = *job_;
                }
                auto path = lookup(target, automation, shells);
                std::lock_guard lock(mutex_);
                job_.reset();
                if (waiting_) result_ = std::move(path);
                condition_.notify_all();
            }
        }
        CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    HWND owner_;
    HANDLE activity_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool stopping_ = false;
    bool waiting_ = false;
    std::optional<FocusTarget> job_;
    std::optional<std::filesystem::path> result_;
    HWND follow_frame_ = nullptr;
    unsigned follow_generation_ = 0;
    std::filesystem::path last_followed_;
    std::optional<FollowResult> follow_result_;
};

bool key_held(int key) {
    return GetAsyncKeyState(key) < 0;
}

bool matches(PreviewHotkey hotkey, const KBDLLHOOKSTRUCT& key) {
    if (hotkey == PreviewHotkey::disabled || (key.flags & LLKHF_ALTDOWN) ||
        key_held(VK_MENU) || key_held(VK_LWIN) || key_held(VK_RWIN)) {
        return false;
    }
    const bool ctrl = key_held(VK_CONTROL);
    const bool shift = key_held(VK_SHIFT);
    return (hotkey == PreviewHotkey::space && !ctrl && !shift) ||
           (hotkey == PreviewHotkey::ctrl_space && ctrl && !shift) ||
           (hotkey == PreviewHotkey::ctrl_shift_space && ctrl && shift);
}

PreviewHotkey read_hotkey() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, settings_key, hotkey_value, RRF_RT_REG_DWORD,
                     nullptr, &value, &bytes) == ERROR_SUCCESS &&
        value <= static_cast<DWORD>(PreviewHotkey::disabled)) {
        return static_cast<PreviewHotkey>(value);
    }
    return PreviewHotkey::space;
}

bool chord_available(HWND owner, PreviewHotkey hotkey) {
    if (hotkey == PreviewHotkey::space || hotkey == PreviewHotkey::disabled) return true;
    const UINT modifiers = MOD_CONTROL | (hotkey == PreviewHotkey::ctrl_shift_space ? MOD_SHIFT : 0);
    // Probe existing system hotkeys without owning a global shortcut outside Explorer.
    if (!RegisterHotKey(owner, hotkey_probe_id, modifiers, VK_SPACE)) return false;
    UnregisterHotKey(owner, hotkey_probe_id);
    return true;
}

} // namespace

struct ExplorerIntegration::Impl {
    struct PreviewRequest {
        FocusTarget target;
        std::filesystem::path path;
    };

    Impl(HWND owner, PreviewCallback open, SupportsFile supports, PreviewCallback follow)
        : owner(owner), open_preview(std::move(open)), supports_file(std::move(supports)),
          follow_selection(std::move(follow)) {}

    ~Impl() { stop_hook(); }

    void stop_hook() {
        if (hook_thread.joinable()) {
            PostThreadMessageW(hook_thread_id, WM_QUIT, 0, 0);
            hook_thread.join();
        }
        // An unresponsive Shell COM server can outlive its input deadline. The inspector
        // owns its state until the RPC returns, so it cannot block window destruction.
        if (inspection) inspection->stop();
        inspection.reset();
        space_down = false;
        space_consumed = false;
        preview.reset();
        follow_enabled = false;
        source_frame = nullptr;
        source_path.clear();
    }

    bool start() {
        const auto configured = read_hotkey();
        const bool available = chord_available(owner, configured);
        if (!available) return false;
        if (configured != PreviewHotkey::disabled && !start_hook()) return false;
        active_hotkey = configured;
        return true;
    }

    bool start_hook() {
        if (hook_thread.joinable()) return true;
        try {
            inspection = FocusInspection::create(owner);
            if (!inspection) return false;
            std::promise<bool> ready;
            auto started = ready.get_future();
            hook_thread = std::thread([this, ready = std::move(ready)]() mutable {
                MSG message{};
                PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
                hook_thread_id = GetCurrentThreadId();
                instance = this;
                const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &keyboard_hook,
                                                    GetModuleHandleW(nullptr), 0);
                ready.set_value(hook != nullptr);
                if (hook) {
                    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
                    UnhookWindowsHookEx(hook);
                }
                instance = nullptr;
            });
            if (started.get()) return true;
            hook_thread.join();
        } catch (const std::system_error&) {
            // An unavailable worker leaves the foreground application's keys alone.
        }
        if (inspection) inspection->stop();
        inspection.reset();
        return false;
    }

    static LRESULT CALLBACK keyboard_hook(int code, WPARAM wparam, LPARAM lparam) {
        if (code == HC_ACTION && instance) {
            const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
            if (key.vkCode == VK_SPACE && !(key.flags & LLKHF_LOWER_IL_INJECTED)) {
                try {
                    if (instance->consume_space(wparam, key)) return 1;
                } catch (...) {
                    // Do not let a failed inspection eat a press or unwind through USER32.
                }
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    bool consume_space(WPARAM message, const KBDLLHOOKSTRUCT& key) {
        if (message == WM_KEYUP || message == WM_SYSKEYUP) {
            space_down = false;
            return std::exchange(space_consumed, false);
        }
        if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return false;
        if (std::exchange(space_down, true)) return space_consumed;
        const auto shortcut = active_hotkey.load();
        if (!matches(shortcut, key)) return false;

        const auto target = explorer_focus();
        if (!target || !inspection) return false;
        auto path = inspection->request(*target);
        if (!path || !supports_file(*path) || shortcut != active_hotkey.load() || explorer_focus() != target) {
            return false;
        }

        std::lock_guard lock(preview_mutex);
        if (preview) return false;
        preview = PreviewRequest{*target, std::move(*path)};
        space_consumed = PostMessageW(owner, preview_message, 0, 0) != FALSE;
        if (!space_consumed) preview.reset();
        return space_consumed;
    }

    HWND owner;
    PreviewCallback open_preview;
    SupportsFile supports_file;
    PreviewCallback follow_selection;
    std::shared_ptr<FocusInspection> inspection;
    std::thread hook_thread;
    DWORD hook_thread_id = 0;
    std::atomic<PreviewHotkey> active_hotkey{PreviewHotkey::disabled};
    bool space_down = false;
    bool space_consumed = false;
    std::mutex preview_mutex;
    std::optional<PreviewRequest> preview;
    bool follow_enabled = false;
    HWND source_frame = nullptr;
    std::filesystem::path source_path;
    static thread_local Impl* instance;
};

thread_local ExplorerIntegration::Impl* ExplorerIntegration::Impl::instance = nullptr;

ExplorerIntegration::ExplorerIntegration(HWND owner, PreviewCallback open_preview, SupportsFile supports_file,
                                         PreviewCallback follow_selection)
    : impl_(std::make_unique<Impl>(owner, std::move(open_preview), std::move(supports_file),
                                  std::move(follow_selection))) {}

ExplorerIntegration::~ExplorerIntegration() = default;

bool ExplorerIntegration::start() {
    return impl_->start();
}

bool ExplorerIntegration::handle_message(UINT message, WPARAM, LPARAM) {
    if (message == follow_message) {
        if (!impl_->follow_enabled || !impl_->inspection) return true;
        const auto path = impl_->inspection->take_followed_file();
        if (path && impl_->supports_file(*path)) impl_->follow_selection(*path);
        return true;
    }
    if (message != preview_message) return false;
    std::optional<Impl::PreviewRequest> preview;
    {
        std::lock_guard lock(impl_->preview_mutex);
        preview = std::exchange(impl_->preview, std::nullopt);
    }
    if (preview && explorer_focus() == preview->target) {
        impl_->source_frame = preview->target.frame;
        impl_->source_path = preview->path;
        if (impl_->follow_enabled && impl_->inspection) {
            impl_->inspection->follow(impl_->source_frame, impl_->source_path);
        }
        impl_->open_preview(preview->path);
    }
    return true;
}

PreviewHotkey ExplorerIntegration::hotkey() const {
    return impl_->active_hotkey.load();
}

bool ExplorerIntegration::set_hotkey(PreviewHotkey shortcut) {
    if (shortcut != hotkey() && !chord_available(impl_->owner, shortcut)) return false;
    if (shortcut != PreviewHotkey::disabled && !impl_->start_hook()) return false;
    const DWORD value = static_cast<DWORD>(shortcut);
    if (RegSetKeyValueW(HKEY_CURRENT_USER, settings_key, hotkey_value, REG_DWORD,
                        &value, sizeof(value)) != ERROR_SUCCESS) {
        return false;
    }
    impl_->active_hotkey = shortcut;
    if (shortcut == PreviewHotkey::disabled) impl_->stop_hook();
    return true;
}

void ExplorerIntegration::set_follow_selection(bool enabled) {
    const bool follow = enabled && impl_->follow_selection && source_explorer();
    if (std::exchange(impl_->follow_enabled, follow) != follow && impl_->inspection) {
        impl_->inspection->follow(follow ? impl_->source_frame : nullptr,
                                  follow ? impl_->source_path : std::filesystem::path{});
    }
    if (!enabled) {
        impl_->source_frame = nullptr;
        impl_->source_path.clear();
    }
}

HWND ExplorerIntegration::source_explorer() const {
    return IsWindow(impl_->source_frame) ? impl_->source_frame : nullptr;
}

} // namespace viewery
