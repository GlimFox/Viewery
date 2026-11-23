#include <windows.h>

#include <string>
#include <vector>

namespace {
std::vector<std::wstring> popupCaptions;
unsigned preferenceWriteAttempts = 0;

LSTATUS WINAPI ignorePreferenceWrite(HKEY, LPCWSTR, LPCWSTR, DWORD, LPCVOID, DWORD)
{
    ++preferenceWriteAttempts;
    return ERROR_SUCCESS;
}

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
#define RegSetKeyValueW ignorePreferenceWrite
#include "main_ui_source.inc"
#undef RegSetKeyValueW
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

struct ButtonActivity { unsigned positions = 0, paints = 0; };

LRESULT CALLBACK countButtonActivity(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR value)
{
    auto& count = *reinterpret_cast<ButtonActivity*>(value);
    if (message == WM_WINDOWPOSCHANGED) ++count.positions;
    if (message == WM_PAINT) ++count.paints;
    return DefSubclassProc(window, message, wparam, lparam);
}

void finishPaint(HWND parent)
{
    pump();
    UpdateWindow(parent);
    pump();
}

void checkStableLayout(viewery::App& app)
{
    using namespace viewery;
    app.layout();
    RedrawWindow(app.window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    finishPaint(app.window_);
    ButtonActivity count;
    std::vector<HWND> watched;
    for (const auto id : {Control::open, Control::catalog, Control::panelPosition, Control::renameStudio,
                          Control::info, Control::exportGif, Control::settings, Control::more,
                          Control::catalogSelect, Control::catalogArtists}) {
        HWND child = app.buttons_[static_cast<UINT>(id) - static_cast<UINT>(Control::open)];
        if (child && IsWindowVisible(child)) {
            require(SetWindowSubclass(child, countButtonActivity, 72, reinterpret_cast<DWORD_PTR>(&count)) != FALSE,
                    "native toolbar can be observed without changing product behavior");
            watched.push_back(child);
        }
    }
    for (int index = 0; index < 12; ++index) {
        app.layout();
        finishPaint(app.window_);
    }
    for (HWND child : watched) RemoveWindowSubclass(child, countButtonActivity, 72);
    require(count.positions == 0, "unchanged layout does not reposition or change visibility of toolbar buttons");
    require(count.paints == 0, "unchanged layout does not repaint stable toolbar buttons");

    count = {};
    watched.clear();
    for (const auto id : {Control::open, Control::catalog, Control::panelPosition, Control::renameStudio,
                          Control::info, Control::exportGif, Control::settings, Control::more,
                          Control::catalogSelect, Control::catalogArtists}) {
        HWND child = app.buttons_[static_cast<UINT>(id) - static_cast<UINT>(Control::open)];
        if (child && IsWindowVisible(child)) {
            SetWindowSubclass(child, countButtonActivity, 72, reinterpret_cast<DWORD_PTR>(&count));
            watched.push_back(child);
        }
    }
    const int originalThumb = app.thumb_;
    POINT pointer{app.catalogRect_.left + app.dp(38), app.catalogRect_.top + app.catalogHeaderHeight() + app.dp(25)};
    ClientToScreen(app.window_, &pointer);
    for (int index = 0; index < 8; ++index) {
        const int delta = index % 2 == 0 ? WHEEL_DELTA : -WHEEL_DELTA;
        app.wheel(MAKEWPARAM(MK_CONTROL, static_cast<WORD>(delta)), MAKELPARAM(pointer.x, pointer.y));
        finishPaint(app.window_);
    }
    for (HWND child : watched) RemoveWindowSubclass(child, countButtonActivity, 72);
    require(app.thumb_ == originalThumb, "Ctrl-wheel changes thumbnails in both directions");
    require(count.positions == 0, "Ctrl-wheel does not move stable toolbar buttons");
    require(count.paints <= 1, "Ctrl-wheel does not repeatedly repaint stable toolbar buttons");
    require(preferenceWriteAttempts > 0, "preference saves are intercepted instead of touching user registry");
}

void checkNavigation(viewery::App& app, const std::vector<std::filesystem::path>& paths)
{
    using namespace viewery;
    const auto viewed = app.path_;
    const auto selected = app.selectedFiles_;
    const auto panelWidth = app.catalogWidth_;
    app.catalogWidth_ = 400;
    app.panel_ = Panel::left;
    app.cards_ = true;
    app.layout();
    require(app.columns() >= 2, "grid fixture has multiple thumbnail columns");
    app.openMedia(paths[4]);
    app.key(VK_RIGHT, false);
    require(app.path_ == paths[5], "ordinary Right previews the next file instead of panning the image");
    app.key(VK_LEFT, false);
    require(app.path_ == paths[4], "ordinary Left previews the previous file");
    const int columns = app.columns();
    app.key(VK_UP, false);
    require(app.path_ == paths[static_cast<std::size_t>(4 - columns)], "Up moves one visible grid row backwards");
    app.key(VK_DOWN, false);
    require(app.path_ == paths[4], "Down moves one visible grid row forwards");
    app.key(VK_RIGHT, true);
    require(app.path_ == paths[4], "Alt Right pans the image without changing the viewed file");
    app.panel_ = Panel::bottom;
    app.layout();
    app.key(VK_DOWN, false);
    require(app.path_ == paths[5], "Down advances one file in a horizontal filmstrip");
    app.key(VK_UP, false);
    require(app.path_ == paths[4], "Up goes back one file in a horizontal filmstrip");
    app.key(VK_RIGHT, false);
    require(app.path_ == paths[5], "Right also advances one file in a horizontal filmstrip");
    app.key(VK_LEFT, false);
    require(app.path_ == paths[4], "Left also goes back in a horizontal filmstrip");
    require(app.selectedFiles_ == selected, "arrow navigation preserves the independent checked selection");
    app.catalogWidth_ = panelWidth;
    app.panel_ = Panel::left;
    app.openMedia(viewed);
    app.layout();
}

void checkCanvasGestures(viewery::App& app)
{
    using namespace viewery;
    app.mode_ = Mode::full;
    app.layout();
    RECT original{};GetWindowRect(app.window_, &original);
    POINT point{(app.mediaRect_.left + app.mediaRect_.right) / 2,
                (app.mediaRect_.top + app.mediaRect_.bottom) / 2};
    app.beginCanvasGesture(point, false);
    app.moveCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    require(app.gesture_ == CanvasGesture::pan, "full-view left drag pans the canvas");
    RECT after{};GetWindowRect(app.window_, &after);
    require(EqualRect(&original, &after) != FALSE, "full-view canvas drag leaves the application window in place");
    app.endCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});

    app.beginCanvasGesture(point, true);
    app.moveCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    require(app.gesture_ == CanvasGesture::moveWindow, "Ctrl left drag moves the full-view window");
    GetWindowRect(app.window_, &after);
    require(after.left != original.left || after.top != original.top, "full-view Ctrl drag changes the native window position");
    app.endCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    SetWindowPos(app.window_, nullptr, original.left, original.top, spanX(original), spanY(original), SWP_NOZORDER | SWP_NOACTIVATE);

    app.switchMode(Mode::quick);
    app.layout();
    point = {(app.mediaRect_.left + app.mediaRect_.right) / 2,
             (app.mediaRect_.top + app.mediaRect_.bottom) / 2};
    app.beginCanvasGesture(point, true);
    app.moveCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    require(app.gesture_ == CanvasGesture::pan, "Quick View keeps Ctrl left drag for canvas panning");
    app.endCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    app.beginCanvasGesture(point, false);
    app.moveCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    require(app.gesture_ == CanvasGesture::moveWindow, "Quick View keeps ordinary left drag for moving the window");
    app.endCanvasGesture({point.x + app.dp(30), point.y + app.dp(20)});
    app.switchMode(Mode::full);
    SetWindowPos(app.window_, nullptr, original.left, original.top, spanX(original), spanY(original), SWP_NOZORDER | SWP_NOACTIVATE);
    app.layout();
}

void checkResizablePanels(viewery::App& app)
{
    using namespace viewery;
    app.openStudio();
    app.layout();
    POINT grip{(app.detailsDivider_.left + app.detailsDivider_.right) / 2,
               app.detailsDivider_.top + app.dp(50)};
    app.click(grip, 0);
    require(app.resizingDetails_, "right tools panel exposes a draggable separator");
    const POINT moved{app.client_.right - app.dp(450), grip.y};
    SendMessageW(app.window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(moved.x, moved.y));
    require(app.detailsWidth_ == 450 && spanX(app.detailsRect_) == app.dp(450), "dragging the right separator adjusts the tools panel width");
    ReleaseCapture();
    require(!app.resizingDetails_, "releasing capture ends the right-panel drag");

    grip = {(app.divider_.left + app.divider_.right) / 2, app.divider_.top + app.dp(30)};
    app.click(grip, 0);
    require(app.resizingPanel_, "left catalog separator remains draggable");
    SendMessageW(app.window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(app.dp(300), grip.y));
    require(app.catalogWidth_ == 300 && spanX(app.catalogRect_) == app.dp(300), "dragging the catalog separator changes its width");
    ReleaseCapture();

    app.panel_ = Panel::bottom;
    app.layout();
    grip = {app.divider_.left + app.dp(35), (app.divider_.top + app.divider_.bottom) / 2};
    app.click(grip, 0);
    require(app.resizingPanel_, "bottom catalog exposes a draggable separator");
    SendMessageW(app.window_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(grip.x, app.catalogRect_.bottom - app.dp(205)));
    require(app.catalogHeight_ == 205 && spanY(app.catalogRect_) == app.dp(205), "dragging the bottom separator changes the filmstrip height");
    ReleaseCapture();
    app.panel_ = Panel::left;
    app.catalogWidth_ = 250;
    app.catalogHeight_ = 156;
    app.detailsWidth_ = 510;
    app.layout();
}

void checkQuickImageSpace(viewery::App& app)
{
    using namespace viewery;
    app.switchMode(Mode::quick);
    app.layout();
    app.quickSpaceGesture_.reset();
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, 1);
    SendMessageW(app.window_, WM_KEYUP, VK_SPACE, 1);
    require(IsWindowVisible(app.window_) != FALSE, "one Space keeps an image preview open");
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, static_cast<LPARAM>(1ULL << 30));
    require(IsWindowVisible(app.window_) != FALSE, "held Space does not close Quick View through key repeat");
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, 1);
    SendMessageW(app.window_, WM_KEYUP, VK_SPACE, 1);
    require(!IsWindowVisible(app.window_) && !app.player_ && app.path_.empty(), "a rapid second physical Space closes Quick View and releases media");
}

void checkVideoSpace(viewery::App& app, const std::filesystem::path& video)
{
    using namespace viewery;
    app.player_.reset();
    app.player_ = std::make_unique<MediaPlayer>();
    require(app.player_->initialize(nullptr), "real playback engine initializes with null output for isolated key testing");
    require(app.player_->open(video, true), "real test video opens paused");
    require(wait([&] { app.playback_ = app.player_->poll();return app.playback_.ready; }), "test video reaches a ready playback state");
    app.kind_ = MediaKind::video;
    app.path_ = video;
    app.mode_ = Mode::full;
    app.layout();
    const auto before = app.playback_.position;
    app.key(VK_RIGHT, true);
    require(wait([&] { app.playback_ = app.player_->poll();return app.playback_.paused && app.playback_.position > before + 0.02; }),
            "Alt Right steps one video frame and pauses without changing files");
    require(app.path_ == video, "frame stepping preserves the viewed video");
    app.switchMode(Mode::quick);
    ShowWindow(app.window_, SW_SHOWNOACTIVATE);
    app.layout();
    app.quickSpaceGesture_.reset();
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, 1);
    SendMessageW(app.window_, WM_KEYUP, VK_SPACE, 1);
    require(wait([&] { app.playback_ = app.player_->poll();return !app.playback_.paused; }), "Space resumes the paused video in Quick View");
    app.quickSpaceGesture_.reset();
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, 1);
    SendMessageW(app.window_, WM_KEYUP, VK_SPACE, 1);
    require(wait([&] { app.playback_ = app.player_->poll();return app.playback_.paused; }), "a separate Space pauses the playing Quick View video");
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, static_cast<LPARAM>(1ULL << 30));
    require(IsWindowVisible(app.window_) && app.player_->poll().paused, "video key repeat neither resumes playback nor closes Quick View");
    app.quickSpaceGesture_.reset();
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, 1);
    SendMessageW(app.window_, WM_KEYUP, VK_SPACE, 1);
    require(wait([&] { app.playback_ = app.player_->poll();return !app.playback_.paused; }), "first press of a rapid video pair controls playback");
    SendMessageW(app.window_, WM_KEYDOWN, VK_SPACE, 1);
    SendMessageW(app.window_, WM_KEYUP, VK_SPACE, 1);
    require(!IsWindowVisible(app.window_) && !app.player_, "second rapid Space closes the playing video preview");
}
}

int wmain(int argc, wchar_t** argv)
{
    using namespace viewery;
    namespace fs = std::filesystem;
    if (argc != 2 && argc != 3) return 2;

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
            // A dormant integration object provides the ordinary lifecycle API;
            // start() is deliberately never called, so no global hook is installed.
            app.explorer_ = std::make_unique<ExplorerIntegration>(app.window_,
                [](const fs::path&) {}, [](const fs::path&) { return true; });
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
            checkNavigation(app, paths);

            app.studio_ = std::make_unique<RenameStudio>(app.window_, libraryMessage,
                [&] { app.beforeLibraryChanges(); }, root / L"collection.sqlite3");
            app.openStudio();
            require(wait([&] { return !app.studio_->busy(); }), "Studio indexes its explicit temporary database");
            require(app.detail_ == Detail::renameStudio && IsWindowVisible(app.studio_->window()),
                "Rename Studio opens as right child panel");
            require(descendant(app.studio_->window(), 1037) != nullptr && descendant(app.studio_->window(), 1043) != nullptr,
                "integrated Studio creates native preview and history lists");
            const auto checkboxButton = app.buttons_[static_cast<UINT>(Control::catalogSelect) - static_cast<UINT>(Control::open)];
            const auto artistButton = app.buttons_[static_cast<UINT>(Control::catalogArtists) - static_cast<UINT>(Control::open)];
            const auto settingsButton = app.buttons_[static_cast<UINT>(Control::settings) - static_cast<UINT>(Control::open)];
            require(IsWindowVisible(checkboxButton) && IsWindowVisible(artistButton) && IsWindowVisible(settingsButton),
                "catalog has dedicated checkbox and artist controls and toolbar has Settings");
            const bool previousSelectionMode = app.selectMode_;
            app.command(static_cast<UINT>(Control::catalogSelect));
            require(app.selectMode_ != previousSelectionMode, "catalog checkbox button toggles selection mode directly");
            app.command(static_cast<UINT>(Control::catalogSelect));
            require(app.selectMode_ == previousSelectionMode, "catalog selection mode button toggles back without changing files");
            app.command(static_cast<UINT>(Control::catalogArtists));
            require(std::ranges::any_of(popupCaptions, [](const auto& label) { return label.find(L"Все файлы") != std::wstring::npos; }) &&
                    std::ranges::any_of(popupCaptions, [](const auto& label) { return label.find(L"художников") != std::wstring::npos; }),
                "artist control exposes named artist grouping and a clear All files action");
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
            app.openStudio();
            require(wait([&] { return !app.studio_->busy(); }), "Studio is idle before layout repaint regression checks");
            checkStableLayout(app);
            checkCanvasGestures(app);
            checkResizablePanels(app);
            checkQuickImageSpace(app);
            if (argc == 3) checkVideoSpace(app, fs::absolute(argv[2]));
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
