#include <windows.h>

#include <string>
#include <vector>

namespace {
std::vector<std::wstring> popupCaptions;

// Inspect the real menu and cancel it. A popup on a non-input desktop cannot
// reliably receive mouse input; Shell, clipboard and file actions stay unused.
BOOL WINAPI inspectPopup(HMENU menu, UINT, int, int, int, HWND, const RECT*)
{
    popupCaptions.clear();
    for (int index = 0; index < GetMenuItemCount(menu); ++index) {
        wchar_t value[1024]{};
        GetMenuStringW(menu, index, value, static_cast<int>(std::size(value)), MF_BYPOSITION);
        popupCaptions.emplace_back(value);
    }
    return 0;
}
}

#define TrackPopupMenu inspectPopup
#include "main_ui_source.inc"
#undef TrackPopupMenu

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    std::cout << (condition ? "OK " : "FAIL ") << message << std::endl;
    if (!condition) throw std::runtime_error(message);
}

void pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool wait(auto condition)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < end) {
        pump();
        if (condition()) return true;
        Sleep(10);
    }
    return false;
}

RECT localRect(HWND parent, HWND child)
{
    RECT rect{};
    GetWindowRect(child, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

std::wstring windowText(HWND window)
{
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
    return value;
}

HWND descendant(HWND parent, int id)
{
    struct Find { int id; HWND window = nullptr; } find{id};
    EnumChildWindows(parent, [](HWND window, LPARAM value) -> BOOL {
        auto& result = *reinterpret_cast<Find*>(value);
        if (GetDlgCtrlID(window) != result.id) return TRUE;
        result.window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&find));
    return find.window;
}
}

int wmain(int argc, wchar_t** argv)
{
    using namespace viewery;
    namespace fs = std::filesystem;
    if (argc != 2) return 2;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    const auto desktopName = L"VieweryMainUiTest-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        const DWORD error = GetLastError();
        if (desktop) CloseDesktop(desktop);
        std::cout << "SKIP Private desktop unavailable (Win32 " << error << "). No viewer or database opened.\n";
        return 77;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        SetThreadDesktop(previous);
        CloseDesktop(desktop);
        return 3;
    }

    int result = 0;
    try {
        const auto root = fs::absolute(argv[1]) / (L"run-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        CapturedFrame frame;
        frame.width = frame.height = 16;
        frame.bgra.resize(16 * 16 * 4, 255);
        for (std::size_t at = 0; at < frame.bgra.size(); at += 4) {
            frame.bgra[at] = 127;
            frame.bgra[at + 1] = 63;
        }
        const auto fixture = root / L"fixture.png";
        require(saveFramePng(fixture, frame), "valid PNG fixture is generated without external media tools");
        std::vector<fs::path> paths;
        for (int index = 0; index < 20; ++index) {
            const auto path = root / (L"image" + std::to_wstring(index + 1) + L".png");
            fs::copy_file(fixture, path);
            paths.push_back(path);
        }

        require(App::registerClasses(GetModuleHandleW(nullptr)), "native viewer classes register on private desktop");
        {
            // Do not call start(): no singleton, preference load, Explorer hook,
            // tray icon or automatic update request belongs in this test.
            App app(GetModuleHandleW(nullptr));
            require(app.makeViewer(), "viewer creates without application startup or user settings");
            ShowWindow(app.window_, SW_SHOWNOACTIVATE);
            app.directory_ = root;
            app.path_ = paths[0];
            app.kind_ = MediaKind::image;
            for (const auto& path : paths) {
                CatalogFile file;
                file.path = path;
                file.name = path.filename().wstring();
                file.extension = L".png";
                app.directoryFiles_.push_back(file);
            }
            app.files_ = app.directoryFiles_;
            app.layout();

            app.selectMode_ = true;
            auto tile = app.itemRect(1);
            app.click({tile.left + app.dp(10), tile.top + app.dp(10)}, 0);
            require(app.selectedFiles_.contains(paths[1]) && app.path_ == paths[0],
                "checkbox selects another file without opening it");
            app.resumePlayback_ = PlaybackState{};
            app.resumePlayback_->position = 42;
            app.click({(tile.left + tile.right) / 2, (tile.top + tile.bottom) / 2}, 0);
            require(app.path_ == paths[1] && app.selectedFiles_.contains(paths[1]) && app.selectedFiles_.size() == 1,
                "image click in checkbox mode previews file and preserves selection");
            require(!app.resumePlayback_, "previewing a different file clears previous pending playback restoration");
            tile = app.itemRect(2);
            app.click({(tile.left + tile.right) / 2, (tile.top + tile.bottom) / 2}, 0);
            require(app.path_ == paths[2] && app.selectedFiles_.contains(paths[1]) && app.selectedFiles_.size() == 1,
                "previewing an unselected image preserves checkbox selection");
            app.selectMode_ = false;
            tile = app.itemRect(1);
            app.click({tile.left + app.dp(10), tile.top + app.dp(10)}, 0);
            require(app.selectedFiles_.empty() && app.path_ == paths[2],
                "visible selected checkbox toggles outside dedicated checkbox mode");
            app.selectCatalog(2, 0, false);
            require(app.selectedFiles_.empty() && app.path_ == paths[2],
                "ordinary preview does not create an implicit checked selection");
            app.selectCatalog(3, MK_CONTROL, false);
            app.selectCatalog(6, MK_SHIFT, false);
            require(app.selectedFiles_.size() == 4 && app.path_ == paths[2],
                "Ctrl and Shift selection remain independent from viewed file");

            app.studio_ = std::make_unique<RenameStudio>(app.window_, libraryMessage,
                [&] { app.beforeLibraryChanges(); }, root / L"collection.sqlite3");
            app.openStudio();
            require(wait([&] { return !app.studio_->busy(); }), "Studio indexes its explicit temporary database");
            require(app.detail_ == Detail::renameStudio && IsWindowVisible(app.studio_->window()),
                "Rename Studio opens as right child panel");
            require(descendant(app.studio_->window(), 1037) != nullptr && descendant(app.studio_->window(), 1043) != nullptr,
                "integrated Studio creates native preview and history lists");
            app.command(libraryOpenStudio);
            require(app.detail_ == Detail::renameStudio && IsWindowVisible(app.studio_->window()),
                "rename menu command keeps an already open Studio open");
            app.command(static_cast<UINT>(Control::renameStudio));
            require(app.detail_ == Detail::none && !IsWindowVisible(app.studio_->window()) && app.renameEnabled_,
                "toolbar button closes Studio without disabling module");
            app.openStudio();
            app.key(VK_ESCAPE, false);
            require(app.detail_ == Detail::none && app.renameEnabled_, "Escape closes Studio without disabling module");
            app.openStudio();

            const auto beforeScroll = app.scroll_;
            POINT studioPoint{app.detailsRect_.left + app.dp(20), app.detailsRect_.top + app.dp(180)};
            ClientToScreen(app.window_, &studioPoint);
            app.wheel(MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(studioPoint.x, studioPoint.y));
            require(app.scroll_ == beforeScroll, "wheel over Studio preserves catalog scroll");
            app.panel_ = Panel::bottom;
            app.layout();
            const auto search = localRect(app.window_, app.catalogSearch_);
            require(search.right <= app.catalogRect_.right && search.right <= app.detailsRect_.left,
                "bottom catalog search ends before Studio");
            app.panel_ = Panel::left;
            SetWindowPos(app.window_, nullptr, 0, 0, 670, 475, SWP_NOZORDER | SWP_NOACTIVATE);
            app.layout();
            require(spanX(app.mediaRect_) > 0 && spanX(app.detailsRect_) >= app.dp(380),
                "small window retains usable Studio width and canvas");

            for (UINT dpi : {96u, 120u, 144u}) {
                app.dpi_ = dpi;
                app.updateFonts();
                SetWindowPos(app.window_, nullptr, 0, 0, app.dp(1110), app.dp(710), SWP_NOZORDER | SWP_NOACTIVATE);
                app.layout();
                const auto button = app.buttons_[static_cast<UINT>(Control::renameStudio) - static_cast<UINT>(Control::open)];
                const auto value = windowText(button);
                HDC dc = GetDC(button);
                const auto oldFont = SelectObject(dc, app.font_);
                SIZE size{};
                GetTextExtentPoint32W(dc, value.data(), static_cast<int>(value.size()), &size);
                SelectObject(dc, oldFont);
                ReleaseDC(button, dc);
                RECT bounds{};
                GetClientRect(button, &bounds);
                require(size.cx + app.dp(6) <= bounds.right, "toolbar Rename caption fits tested DPI");
                app.detail_ = Detail::none;
                app.playback_.width = 1344;
                app.playback_.height = 1728;
                app.playback_.videoCodec = L"png";
                app.status_.clear();
                app.layout();
                require(app.filenameRect_.left >= app.dp(185), "photo footer reserves space for dimensions and format");
                app.openStudio();
            }

            app.dpi_ = 96;
            app.updateFonts();
            SetWindowPos(app.window_, nullptr, 0, 0, 1110, 710, SWP_NOZORDER | SWP_NOACTIVATE);
            app.panel_ = Panel::left;
            app.layout();
            tile = app.itemRect(4);
            POINT context{(tile.left + tile.right) / 2, (tile.top + tile.bottom) / 2};
            ClientToScreen(app.window_, &context);
            SendMessageW(app.window_, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(app.window_), MAKELPARAM(context.x, context.y));
            const auto contains = [](std::wstring_view value) {
                return std::ranges::any_of(popupCaptions, [&](const auto& caption) { return caption.find(value) != std::wstring::npos; });
            };
            require(contains(L"Свойства") && contains(L"Проводнике"), "thumbnail menu contains Properties and containing folder");
            require(!contains(L"Панель слева") && !contains(L"Размер миниатюр"), "thumbnail menu excludes catalog configuration");
            require(app.path_ == paths[2], "file menu leaves viewed media unchanged");
            require(!popupCaptions.empty() && popupCaptions.front() == paths[4].filename().wstring(),
                "file menu targets clicked thumbnail rather than viewed file");
            require(app.catalogItemAt({app.catalogRect_.left + app.dp(20), app.catalogRect_.top + app.dp(57)}) == -1,
                "search and header never hit-test scrolled file content");
            app.fileCommand(filePreview, paths[4]);
            require(app.path_ == paths[4] && app.selectedFiles_.size() == 4, "context preview opens exact file and preserves selection");
            app.fileCommand(fileSelect, paths[8]);
            require(app.selectedFiles_.contains(paths[8]) && app.path_ == paths[4], "context selection toggles exact file without opening it");
            SendMessageW(app.window_, WM_COMMAND, RenameStudio::closeCommandId, 0);
            require(app.detail_ == Detail::none && app.renameEnabled_, "Studio close button hides panel while module remains enabled");
        }
        std::cout << "Main UI checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        result = 1;
    }
    CoUninitialize();
    SetThreadDesktop(previous);
    CloseDesktop(desktop);
    return result;
}
