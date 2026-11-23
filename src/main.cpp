#include "catalog.hpp"
#include "converter.hpp"
#include "explorer.hpp"
#include "frame_image.hpp"
#include "media.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if !defined(_M_X64)
#error Viewery requires a Windows x64 build.
#endif

namespace viewery {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

constexpr wchar_t controllerClass[] = L"Viewery.Controller.1";
constexpr wchar_t viewerClass[] = L"Viewery.Viewer.1";
constexpr wchar_t videoClass[] = L"Viewery.Video.1";
constexpr wchar_t stripClass[] = L"Viewery.Controls.1";
constexpr UINT catalogMessage = WM_APP + 20;
constexpr UINT exportMessage = WM_APP + 21;
constexpr UINT trayMessage = WM_APP + 22;
constexpr UINT promoteMessage = WM_APP + 23;
constexpr ULONG_PTR openMessage = 0x56574552;
constexpr ULONG_PTR quickMessage = 0x56574551;
constexpr UINT refreshTimer = 1;
constexpr UINT canvasClickTimer = 2;
constexpr UINT filenameTimer = 3;

constexpr COLORREF background = RGB(14, 16, 21);
constexpr COLORREF surface = RGB(23, 26, 33);
constexpr COLORREF raised = RGB(36, 40, 52);
constexpr COLORREF border = RGB(44, 48, 61);
constexpr COLORREF textColor = RGB(241, 242, 247);
constexpr COLORREF hint = RGB(149, 155, 170);
constexpr COLORREF accent = RGB(155, 140, 255);
constexpr COLORREF accentHover = RGB(176, 166, 255);
constexpr COLORREF activeSurface = RGB(42, 37, 69);

enum class Mode { full, quick };
enum class Panel { left, bottom, hidden };
enum class Detail { none, information, exportGif };
enum class CanvasGesture { none, pressed, moveWindow, pan };
enum class Control : UINT {
    open = 100, catalog, info, exportGif, more, previous, back, play, step, next, speed,
    zoom, fullScreen, pin, promote, close, compact, balanced, quality, cancel, copy,
    panelPosition, volume, preset, advanced, startExport, capture, counter, timestamp
};

constexpr std::array buttonLabels{
    L"Открыть файл или каталог", L"Показать или скрыть каталог", L"Сведения о файле",
    L"Экспорт медиа", L"Дополнительные команды", L"Предыдущий файл", L"Предыдущий кадр",
    L"Пауза", L"Следующий кадр", L"Следующий файл", L"1×", L"Вписать медиа",
    L"Полноэкранный режим", L"Закрепить поверх окон", L"Открыть полный Viewery", L"Закрыть",
    L"Компактный", L"Сбалансированный", L"Высокое качество", L"Отменить экспорт",
    L"Копировать сведения", L"Переместить каталог влево или вниз", L"Громкость",
    L"Сбалансированный", L"Расширенные настройки", L"Экспортировать",
    L"Стоп-кадр: ЛКМ — копировать, ПКМ — сохранить PNG",
    L"Переключить время и кадры", L"Вставить позицию таймлайна в выбранное поле"
};

enum Menu : UINT {
    openFile = 300, openFolder, panelLeft, panelBottom, panelHidden, listView, cardsView,
    smallThumb, mediumThumb, largeThumb, nameSort, dateSort, sizeSort, typeSort,
    hotkeySpace, hotkeyCtrl, hotkeyCtrlShift, hotkeyOff, quit, resetZoom,
    speedSlow, speedNormal, speedFast, speedDouble, seekBack, seekForward
};

constexpr UINT rangeStartId = 600;
constexpr UINT rangeEndId = 601;
constexpr UINT volumeTrackId = 602;

std::wstring_view iconFor(Control control, bool paused, bool fullscreen, bool bottom)
{
    switch (control) {
    case Control::open: return L"\xE838";
    case Control::catalog: return L"\xE8FD";
    case Control::info: return L"\xE946";
    case Control::exportGif: return L"\xE792";
    case Control::more: return L"\xE712";
    case Control::previous: return L"\xE892";
    case Control::back: return L"\xE76B";
    case Control::play: return paused ? L"\xE768" : L"\xE769";
    case Control::step: return L"\xE76C";
    case Control::next: return L"\xE893";
    case Control::zoom: return L"\xE9A6";
    case Control::fullScreen: return fullscreen ? L"\xE740" : L"\xE92D";
    case Control::pin: return L"\xE718";
    case Control::promote: return L"\xE8A7";
    case Control::close: return L"\xE8BB";
    case Control::panelPosition: return bottom ? L"\xE8A0" : L"\xE8A1";
    case Control::volume: return L"\xE767";
    case Control::capture: return L"\xE722";
    case Control::timestamp: return L"\xE823";
    default: return {};
    }
}

void fill(HDC dc, RECT rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void rounded(HDC dc, RECT rect, COLORREF fillColor, COLORREF edge, int radius)
{
    HBRUSH brush = CreateSolidBrush(fillColor);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void label(HDC dc, std::wstring_view text, RECT rect, COLORREF color = textColor,
           UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
{
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
}

int spanX(RECT rect) { return rect.right - rect.left; }
int spanY(RECT rect) { return rect.bottom - rect.top; }

std::wstring timeLabel(double seconds, bool milliseconds = false)
{
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e9) return L"––:––:––";
    const auto total = milliseconds ? std::llround(seconds * 1000.0) : static_cast<long long>(seconds) * 1000;
    const auto whole = total / 1000;
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%02lld:%02lld:%02lld", whole / 3600, whole / 60 % 60, whole % 60);
    if (milliseconds) swprintf_s(buffer + wcslen(buffer), std::size(buffer) - wcslen(buffer), L".%03lld", total % 1000);
    return buffer;
}

std::wstring decimal(double value, int digits = 1)
{
    wchar_t buffer[32];
    swprintf_s(buffer, L"%.*f", digits, value);
    return buffer;
}

std::wstring localDate(FILETIME filetime)
{
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&filetime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"—";
    wchar_t date[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr);
    return date;
}

void copyText(HWND window, const std::wstring& value)
{
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    if (!memory) return;
    if (auto* destination = static_cast<wchar_t*>(GlobalLock(memory))) {
        std::copy_n(value.c_str(), value.size() + 1, destination);
        GlobalUnlock(memory);
        if (OpenClipboard(window)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
            CloseClipboard();
        }
    }
    if (memory) GlobalFree(memory);
}

std::optional<fs::path> dialogPath(HWND owner, bool folder, const fs::path* target = nullptr, bool png = false)
{
    ComPtr<IFileDialog> dialog;
    const CLSID& clsid = target ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    if (target) {
        if (png) options |= FOS_OVERWRITEPROMPT;
        else options &= ~FOS_OVERWRITEPROMPT;
        const COMDLG_FILTERSPEC format = png ? COMDLG_FILTERSPEC{L"Изображение PNG", L"*.png"}
                                            : COMDLG_FILTERSPEC{L"Анимация GIF", L"*.gif"};
        dialog->SetFileTypes(1, &format);
        dialog->SetDefaultExtension(png ? L"png" : L"gif");
        dialog->SetFileName(target->filename().c_str());
        ComPtr<IShellItem> directory;
        if (SUCCEEDED(SHCreateItemFromParsingName(target->parent_path().c_str(), nullptr,
                                                  IID_PPV_ARGS(&directory)))) dialog->SetFolder(directory.Get());
        dialog->SetTitle(png ? L"Сохранить стоп-кадр" : L"Экспорт видео в GIF");
    } else if (folder) {
        options |= FOS_PICKFOLDERS;
    } else {
        const COMDLG_FILTERSPEC filters[] = {
            {L"Медиафайлы", L"*.jpg;*.jpeg;*.png;*.gif;*.webp;*.bmp;*.tif;*.tiff;*.mp4;*.mkv;*.webm;*.mov;*.avi;*.m4v;*.wmv;*.mpeg;*.mpg;*.mts;*.m2ts;*.ts;*.flv;*.ogv;*.3gp"},
            {L"Все файлы", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    }
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

bool samePath(const fs::path& a, const fs::path& b)
{
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void darkTitle(HWND window)
{
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
}

void menuItem(HMENU menu, UINT command, const wchar_t* name, bool selected = false)
{
    AppendMenuW(menu, selected ? MF_STRING | MF_CHECKED : MF_STRING, command, name);
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App();

    bool start();
    void show(const fs::path& path, Mode mode);
    void choose(bool folder);
    int run();
    static bool registerClasses(HINSTANCE instance);
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK videoProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK stripProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK volumeProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK captureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR id, DWORD_PTR data);

private:
    bool makeViewer();
    void switchMode(Mode mode);
    void promotePreview();
    void hideViewer();
    void updateTitle();
    void openMedia(const fs::path& path);
    void changeDirectory(const fs::path& directory, bool selectFirst);
    void navigate(int direction);
    bool acceptCatalog();
    void requestThumbnails();
    void revealSelection();
    RECT itemRect(std::size_t index) const;
    int columns() const;
    int strideX() const;
    int strideY() const;
    int maxScroll() const;
    void scrollCatalog(int distance);

    int dp(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    void updateFonts();
    void setTooltips();
    void layout();
    void paint();
    void paintCatalog(HDC dc);
    void paintDetails(HDC dc);
    void paintStrip();
    void paintFilename(HDC dc, RECT bounds);
    void paintButton(const DRAWITEMSTRUCT& item);
    LRESULT paintVolume(const NMCUSTOMDRAW& draw);
    void place(Control control, int x, int y, int width, int height, bool enabled = true);
    void refresh();
    void changeFullscreen();
    RECT quickBounds() const;
    void fitQuickSize();
    void saveQuickSize();
    void togglePin();
    void click(POINT point, UINT buttons);
    void beginCanvasGesture(POINT point, bool ctrl);
    void moveCanvasGesture(POINT point);
    void endCanvasGesture(POINT point);
    void cancelCanvasClick();
    void refreshQuickZOrder();
    void setVolume(double value);
    void updateCounter();
    void captureFrame(FrameCapturePurpose purpose);
    void acceptFrame(FrameCaptureResult result);
    bool key(WPARAM code, bool alt);
    void wheel(WPARAM wparam, LPARAM lparam);
    void command(UINT id);
    void openMenu(POINT point, bool tray);
    void catalogMenu(POINT point);
    void speedMenu(POINT point);
    void presetMenu(POINT point);
    void changeShortcut(PreviewHotkey shortcut);
    void publishTray(bool add);
    void loadPreferences();
    void savePreferences();
    bool receiveOpen(const COPYDATASTRUCT& data);

    std::wstring fileInfo() const;
    std::optional<double> readRange(HWND field) const;
    void setRangeValue(HWND field, double seconds);
    void useTimelinePosition();
    void startExport(GifPreset preset);
    void acceptExport();

    LRESULT dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    HINSTANCE instance_;
    HWND controller_ = nullptr;
    HWND window_ = nullptr;
    HWND video_ = nullptr;
    HWND rangeStart_ = nullptr;
    HWND rangeEnd_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND controlsStrip_ = nullptr;
    HWND volumeTrack_ = nullptr;
    std::array<HWND, buttonLabels.size()> buttons_{};
    std::unique_ptr<ExplorerIntegration> explorer_;
    std::unique_ptr<CatalogWorker> catalog_;
    std::unique_ptr<MediaPlayer> player_;
    PlaybackState playback_{};
    std::optional<MediaKind> kind_;
    fs::path path_, directory_;
    WIN32_FILE_ATTRIBUTE_DATA fileMetadata_{};
    bool hasFileMetadata_ = false;
    std::vector<CatalogFile> files_;
    std::map<fs::path, Thumbnail> thumbnails_;
    std::set<fs::path> requested_;
    UINT taskbarCreated_ = 0;
    unsigned generation_ = 0;
    unsigned dpi_ = 96;
    int current_ = -1;
    int scroll_ = 0;
    int thumb_ = 124;
    int catalogWidth_ = 250;
    int catalogHeight_ = 156;
    bool selectFirst_ = false;
    bool cards_ = true;
    CatalogSort sort_ = CatalogSort::name;

    Mode mode_ = Mode::full;
    Panel panel_ = Panel::left;
    Panel lastPanel_ = Panel::left;
    Detail detail_ = Detail::none;
    RECT client_{}, mediaRect_{}, timeline_{}, catalogRect_{}, detailsRect_{}, divider_{},
         playbackRect_{}, statusRect_{}, headerRect_{}, filenameRect_{};
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    RECT quickWindow_{};
    POINT gestureOrigin_{}, gestureLast_{}, pendingClickPoint_{};
    RECT gestureWindow_{};
    ULONGLONG pendingClickTime_ = 0;
    ULONGLONG filenameSince_ = 0;
    CanvasGesture gesture_ = CanvasGesture::none;
    bool gestureCtrl_ = false;
    bool doubleClick_ = false;
    bool filenameOverflows_ = false;
    double lastAudibleVolume_ = 100.0;
    bool onTop_ = false;
    bool fullscreen_ = false;
    bool switchingMode_ = false;
    bool promotionPending_ = false;
    bool hasQuickSize_ = false;
    bool quickFitPending_ = false;
    bool resizingPanel_ = false;
    bool scrubbing_ = false;
    bool exportAdvanced_ = false;
    bool showFrames_ = false;
    bool settingRange_ = false;
    bool rangeEndAutomatic_ = true;
    HWND rangeTarget_ = nullptr;
    fs::path capturePath_;
    GifPreset preset_ = GifPreset::balanced;

    std::wstring status_;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT iconFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    std::jthread exportThread_;
    ConversionResult exportResult_{};
};

App::~App()
{
    exportThread_.request_stop();
    if (exportThread_.joinable()) exportThread_.join();
    explorer_.reset();
    player_.reset();
    catalog_.reset();
    if (window_) DestroyWindow(window_);
    if (controller_) {
        publishTray(false);
        DestroyWindow(controller_);
    }
    if (font_) DeleteObject(font_);
    if (titleFont_) DeleteObject(titleFont_);
    if (iconFont_) DeleteObject(iconFont_);
    if (editBrush_) DeleteObject(editBrush_);
}

bool App::registerClasses(HINSTANCE instance)
{
    WNDCLASSEXW window{sizeof(window)};
    window.hInstance = instance;
    window.lpfnWndProc = windowProc;
    window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window.lpszClassName = controllerClass;
    if (!RegisterClassExW(&window)) return false;
    window.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window.style = CS_DBLCLKS;
    window.lpszClassName = viewerClass;
    if (!RegisterClassExW(&window)) return false;
    window.lpfnWndProc = videoProc;
    window.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window.lpszClassName = videoClass;
    if (!RegisterClassExW(&window)) return false;
    window.lpfnWndProc = stripProc;
    window.hbrBackground = nullptr;
    window.lpszClassName = stripClass;
    return RegisterClassExW(&window) != 0;
}

LRESULT CALLBACK App::windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) return app->dispatch(window, message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK App::videoProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    const HWND viewer = GetParent(window);
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(window, viewer, &point, 1);
        return SendMessageW(viewer, message, wparam, MAKELPARAM(point.x, point.y));
    }
    case WM_MOUSEWHEEL:
        return SendMessageW(viewer, message, wparam, lparam);
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK App::stripProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        if (app) {
            app->paintStrip();
            return 0;
        }
    }
    return videoProc(window, message, wparam, lparam);
}

LRESULT CALLBACK App::volumeProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MOUSEWHEEL) return SendMessageW(GetParent(window), message, wparam, lparam);
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK App::captureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_RBUTTONUP) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(window, &point);
        return SendMessageW(GetParent(window), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
                            MAKELPARAM(point.x, point.y));
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

bool App::start()
{
    controller_ = CreateWindowExW(0, controllerClass, L"Viewery", WS_POPUP, 0, 0, 0, 0,
                                  nullptr, nullptr, instance_, this);
    if (!controller_) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    explorer_ = std::make_unique<ExplorerIntegration>(
        controller_, [this](const fs::path& file) { show(file, Mode::quick); },
        [](const fs::path& file) { return classifyMedia(file).has_value(); },
        [this](const fs::path& file) {
            if (window_ && IsWindowVisible(window_) && !samePath(path_, file)) openMedia(file);
        });
    if (!explorer_->start()) status_ = L"Сочетание Quick View недоступно. Измените его в меню Viewery.";
    loadPreferences();
    publishTray(true);
    return true;
}

bool App::makeViewer()
{
    const INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    window_ = CreateWindowExW(0, viewerClass, L"Viewery", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1110, 710, nullptr, nullptr, instance_, this);
    if (!window_) return false;
    darkTitle(window_);
    dpi_ = GetDpiForWindow(window_);
    video_ = CreateWindowExW(0, videoClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                             0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    if (!video_) return false;
    controlsStrip_ = CreateWindowExW(0, stripClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS,
                                    0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        const auto id = static_cast<UINT_PTR>(Control::open) + index;
        buttons_[index] = CreateWindowExW(0, L"BUTTON", buttonLabels[index],
            WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(id), instance_, nullptr);
    }
    volumeTrack_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"Громкость",
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | TBS_NOTICKS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(volumeTrackId)), instance_, nullptr);
    SendMessageW(volumeTrack_, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
    SendMessageW(volumeTrack_, TBM_SETPAGESIZE, 0, 10);
    if (volumeTrack_) SetWindowSubclass(volumeTrack_, volumeProc, 1, 0);
    const HWND speaker = buttons_[static_cast<UINT>(Control::volume) - static_cast<UINT>(Control::open)];
    if (speaker) SetWindowSubclass(speaker, volumeProc, 1, 0);
    const HWND camera = buttons_[static_cast<UINT>(Control::capture) - static_cast<UINT>(Control::open)];
    if (camera) SetWindowSubclass(camera, captureProc, 1, 0);
    rangeStart_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(rangeStartId)),
                                  instance_, nullptr);
    rangeEnd_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(rangeEndId)),
                                instance_, nullptr);
    SendMessageW(rangeStart_, EM_SETLIMITTEXT, 24, 0);
    SendMessageW(rangeEnd_, EM_SETLIMITTEXT, 24, 0);
    rangeTarget_ = rangeStart_;
    editBrush_ = CreateSolidBrush(raised);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    setTooltips();
    catalog_ = std::make_unique<CatalogWorker>(window_, catalogMessage);
    DragAcceptFiles(window_, TRUE);
    updateFonts();
    return true;
}

void App::setTooltips()
{
    if (!tooltip_) return;
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        if (!buttons_[index] || !*buttonLabels[index]) continue;
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = window_;
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.uId = reinterpret_cast<UINT_PTR>(buttons_[index]);
        tool.lpszText = const_cast<wchar_t*>(buttonLabels[index]);
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void App::show(const fs::path& path, Mode mode)
{
    if (!window_ && !makeViewer()) return;
    if (mode == Mode::quick) explorer_->set_follow_selection(true);
    else if (!path.empty()) explorer_->set_follow_selection(false);
    if (mode_ != mode) switchMode(mode);
    if (!path.empty()) {
        std::error_code error;
        if (fs::is_directory(path, error)) {
            changeDirectory(path, true);
        } else if (!samePath(path_, path) || !player_) {
            openMedia(path);
        }
    }
    updateTitle();
    layout();
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    else if (!IsWindowVisible(window_)) {
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    if (SetForegroundWindow(window_)) SetFocus(window_);
    refreshQuickZOrder();
    SetTimer(window_, refreshTimer, 125, nullptr);
}

void App::switchMode(Mode mode)
{
    if (mode_ == Mode::quick && !fullscreen_) saveQuickSize();
    quickFitPending_ = false;
    cancelCanvasClick();

    RECT bounds{};
    if (mode == Mode::quick) {
        bounds = quickBounds();
    } else {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = std::min(dp(1110), spanX(monitor.rcWork));
        const int height = std::min(dp(710), spanY(monitor.rcWork));
        bounds.left = monitor.rcWork.left + (spanX(monitor.rcWork) - width) / 2;
        bounds.top = monitor.rcWork.top + (spanY(monitor.rcWork) - height) / 2;
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }

    // Leave the media host and player in place. The frame change can deliver
    // WM_SIZE/WM_DPICHANGED synchronously before the new client area is stable.
    switchingMode_ = true;
    detail_ = Detail::none;
    fullscreen_ = false;
    mode_ = mode;
    const LONG_PTR previous = GetWindowLongPtrW(window_, GWL_STYLE);
    const LONG_PTR style = (previous & WS_VISIBLE) | WS_CLIPCHILDREN |
                           (mode == Mode::quick ? WS_POPUP | WS_THICKFRAME : WS_OVERLAPPEDWINDOW);
    SetWindowLongPtrW(window_, GWL_STYLE, style);
    SetWindowPos(window_, onTop_ && mode == Mode::quick ? HWND_TOPMOST : HWND_NOTOPMOST,
                 bounds.left, bounds.top, spanX(bounds), spanY(bounds),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    const UINT dpi = GetDpiForWindow(window_);
    if (dpi && dpi != dpi_) {
        dpi_ = dpi;
        updateFonts();
    }
    switchingMode_ = false;
}

void App::promotePreview()
{
    if (mode_ != Mode::quick || promotionPending_) return;
    promotionPending_ = PostMessageW(window_, promoteMessage, 0, 0) != FALSE;
}

void App::hideViewer()
{
    if (fullscreen_) changeFullscreen();
    if (mode_ == Mode::quick) saveQuickSize();
    KillTimer(window_, refreshTimer);
    KillTimer(window_, filenameTimer);
    promotionPending_ = false;
    cancelCanvasClick();
    if (GetCapture() == window_) ReleaseCapture();
    explorer_->set_follow_selection(false);
    ShowWindow(window_, SW_HIDE);
    player_.reset();
    capturePath_.clear();
    playback_ = {};
    path_.clear();
    kind_.reset();
    ShowWindow(video_, SW_HIDE);
}

void App::updateTitle()
{
    const auto title = path_.empty() ? L"Медиапросмотр" : path_.filename().wstring();
    SetWindowTextW(window_, title.c_str());
}

void App::openMedia(const fs::path& file)
{
    const auto kind = classifyMedia(file);
    if (!kind) {
        status_ = L"Viewery не поддерживает этот формат файла.";
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    cancelCanvasClick();
    filenameSince_ = GetTickCount64();
    if (!exportThread_.joinable()) status_.clear();
    capturePath_.clear();
    rangeTarget_ = rangeStart_;
    rangeEndAutomatic_ = true;
    setRangeValue(rangeStart_, 0);
    setRangeValue(rangeEnd_, -1);
    kind_ = kind;
    path_ = file;
    hasFileMetadata_ = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fileMetadata_) != FALSE;
    playback_ = {};
    if (detail_ == Detail::exportGif && kind_ != MediaKind::video && !exportThread_.joinable()) {
        detail_ = Detail::none;
    }
    if (!samePath(directory_, file.parent_path())) changeDirectory(file.parent_path(), false);
    revealSelection();
    quickFitPending_ = mode_ == Mode::quick && !hasQuickSize_ && !IsWindowVisible(window_);
    if (!player_) {
        player_ = std::make_unique<MediaPlayer>();
        if (!player_->initialize(video_)) {
            status_ = player_->error();
            player_.reset();
        }
    }
    if (player_ && !player_->open(file)) {
        status_ = player_->error();
        player_.reset();
        playback_.failed = true;
    }
    updateTitle();
    layout();
}

void App::changeDirectory(const fs::path& directory, bool selectFirst)
{
    directory_ = directory;
    files_.clear();
    thumbnails_.clear();
    requested_.clear();
    current_ = -1;
    scroll_ = 0;
    selectFirst_ = selectFirst;
    catalog_->scan(directory, ++generation_);
}

void App::choose(bool folder)
{
    if (auto path = dialogPath(window_, folder)) show(*path, Mode::full);
}

void App::navigate(int direction)
{
    if (files_.empty()) return;
    const int last = static_cast<int>(files_.size()) - 1;
    const int next = current_ < 0 ? (direction > 0 ? 0 : last) : std::clamp(current_ + direction, 0, last);
    if (next == current_) return;
    const auto file = files_[static_cast<std::size_t>(next)].path;
    openMedia(file);
}

bool App::acceptCatalog()
{
    auto update = catalog_->take();
    if (update.generation != generation_) return false;
    if (update.scanned) {
        files_ = std::move(update.files);
        sortCatalog(files_, sort_);
        if (std::exchange(selectFirst_, false)) {
            if (!files_.empty()) {
                const auto first = files_.front().path;
                openMedia(first);
            } else {
                if (player_) player_->close();
                path_.clear();
                kind_.reset();
                playback_ = {};
                status_ = L"В этой папке нет поддерживаемых файлов.";
                updateTitle();
            }
        }
        revealSelection();
    }
    for (auto& [path, image] : update.images) {
        if (requested_.contains(path)) thumbnails_.insert_or_assign(std::move(path), std::move(image));
    }
    if (!update.images.empty()) InvalidateRect(window_, &catalogRect_, FALSE);
    return update.scanned;
}

int App::columns() const
{
    if (panel_ == Panel::bottom || !cards_) return 1;
    return std::max(1, (spanX(catalogRect_) - dp(20)) / dp(thumb_));
}

int App::strideX() const
{
    if (panel_ == Panel::bottom) return dp(thumb_);
    return std::max(1, (spanX(catalogRect_) - dp(20)) / columns());
}

int App::strideY() const
{
    return cards_ ? dp(thumb_ * 3 / 4 + 23) : dp(29);
}

RECT App::itemRect(std::size_t index) const
{
    const int count = static_cast<int>(index);
    if (panel_ == Panel::bottom) {
        const int x = catalogRect_.left + dp(10) + count * strideX() - scroll_;
        return {x, catalogRect_.top + dp(31), x + strideX() - dp(7), catalogRect_.bottom - dp(9)};
    }
    const int x = catalogRect_.left + dp(10) + (count % columns()) * strideX();
    const int y = catalogRect_.top + dp(31) + (count / columns()) * strideY() - scroll_;
    return {x, y, x + strideX() - dp(6), y + strideY() - dp(6)};
}

int App::maxScroll() const
{
    const int count = static_cast<int>(files_.size());
    if (panel_ == Panel::bottom) return std::max(0, count * strideX() + dp(12) - spanX(catalogRect_));
    return std::max(0, ((count + columns() - 1) / columns()) * strideY() + dp(39) - spanY(catalogRect_));
}

void App::scrollCatalog(int distance)
{
    scroll_ = std::clamp(scroll_ + distance, 0, maxScroll());
    requestThumbnails();
    InvalidateRect(window_, &catalogRect_, FALSE);
}

void App::revealSelection()
{
    const auto item = std::ranges::find_if(files_, [this](const CatalogFile& file) {
        return samePath(file.path, path_);
    });
    current_ = item == files_.end() ? -1 : static_cast<int>(item - files_.begin());
    if (current_ >= 0 && panel_ != Panel::hidden && !IsRectEmpty(&catalogRect_)) {
        const RECT itemBounds = itemRect(static_cast<std::size_t>(current_));
        if (panel_ == Panel::bottom) {
            if (itemBounds.left < catalogRect_.left) scroll_ += itemBounds.left - catalogRect_.left - dp(8);
            if (itemBounds.right > catalogRect_.right) scroll_ += itemBounds.right - catalogRect_.right + dp(8);
        } else {
            if (itemBounds.top < catalogRect_.top + dp(31)) scroll_ += itemBounds.top - catalogRect_.top - dp(31);
            if (itemBounds.bottom > catalogRect_.bottom) scroll_ += itemBounds.bottom - catalogRect_.bottom + dp(8);
        }
    }
    scroll_ = std::clamp(scroll_, 0, maxScroll());
    requestThumbnails();
}

void App::requestThumbnails()
{
    if (!catalog_ || IsRectEmpty(&catalogRect_) || (!cards_ && panel_ != Panel::bottom)) return;
    const bool bottom = panel_ == Panel::bottom;
    const int stride = bottom ? strideX() : strideY();
    const int across = columns();
    const int first = std::max(0, (scroll_ / stride - 1) * across);
    const int limit = first + ((bottom ? spanX(catalogRect_) : spanY(catalogRect_)) / stride + 4) * across;
    std::vector<fs::path> pending;
    std::set<fs::path> visible;
    for (int index = first; index < limit && index < static_cast<int>(files_.size()); ++index) {
        const auto& file = files_[static_cast<std::size_t>(index)].path;
        visible.insert(file);
        if (!thumbnails_.contains(file)) pending.push_back(file);
    }
    std::erase_if(thumbnails_, [&visible](const auto& item) { return !visible.contains(item.first); });
    if (visible != requested_) {
        requested_ = std::move(visible);
        catalog_->request(std::move(pending), generation_);
    }
}

void App::updateFonts()
{
    HFONT oldFont = font_;
    HFONT oldTitle = titleFont_;
    HFONT oldIcon = iconFont_;
    font_ = CreateFontW(-dp(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    titleFont_ = CreateFontW(-dp(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    iconFont_ = CreateFontW(-dp(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe Fluent Icons");
    for (HWND button : buttons_) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(rangeStart_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(rangeEnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (oldFont) DeleteObject(oldFont);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldIcon) DeleteObject(oldIcon);
}

void App::place(Control control, int x, int y, int width, int height, bool enabled)
{
    const auto index = static_cast<std::size_t>(static_cast<UINT>(control) - static_cast<UINT>(Control::open));
    HWND button = buttons_[index];
    EnableWindow(button, enabled);
    SetWindowPos(button, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

void App::layout()
{
    if (!video_) return;
    GetClientRect(window_, &client_);
    std::array<BOOL, buttonLabels.size()> visible{};
    const auto button = [&](Control id, int x, int y, int width, bool enabled = true) {
        visible[static_cast<std::size_t>(static_cast<UINT>(id) - static_cast<UINT>(Control::open))] = TRUE;
        place(id, x, y, dp(width), dp(32), enabled);
    };
    const bool fullUi = mode_ == Mode::full && !fullscreen_;
    const bool quickUi = mode_ == Mode::quick;
    const bool moving = kind_ && kind_ != MediaKind::image;
    const bool playbackBar = moving || (quickUi && kind_);
    const bool narrowQuick = quickUi && spanX(client_) < dp(570);

    mediaRect_ = client_;
    catalogRect_ = detailsRect_ = divider_ = timeline_ = {};
    playbackRect_ = statusRect_ = headerRect_ = filenameRect_ = {};
    if (fullUi) {
        headerRect_ = {0, 0, client_.right, dp(40)};
        button(Control::open, dp(8), dp(4), 33);
        button(Control::catalog, dp(46), dp(4), 33);
        button(Control::panelPosition, dp(84), dp(4), 33);
        button(Control::info, client_.right - dp(116), dp(4), 33, kind_.has_value());
        button(Control::exportGif, client_.right - dp(78), dp(4), 33,
               kind_ == MediaKind::video || exportThread_.joinable());
        button(Control::more, client_.right - dp(40), dp(4), 33);
        mediaRect_.top = headerRect_.bottom;
        statusRect_ = {0, client_.bottom - dp(28), client_.right, client_.bottom};
        mediaRect_.bottom = statusRect_.top;
    }
    if (playbackBar) {
        const int height = moving ? (narrowQuick ? 84 : 57) : 36;
        playbackRect_ = {0, mediaRect_.bottom - dp(height), client_.right, mediaRect_.bottom};
        mediaRect_.bottom = playbackRect_.top;
    }

    if (fullUi && panel_ == Panel::left) {
        catalogRect_ = mediaRect_;
        catalogRect_.right = dp(std::clamp(catalogWidth_, 172, std::max(172, spanX(client_) * 96 / static_cast<int>(dpi_) - 375)));
        mediaRect_.left = catalogRect_.right + dp(4);
        divider_ = {catalogRect_.right, catalogRect_.top, mediaRect_.left, catalogRect_.bottom};
    }
    const RECT contentRect = mediaRect_;
    if (fullUi && panel_ == Panel::bottom) {
        catalogRect_ = {0, mediaRect_.bottom - dp(catalogHeight_), mediaRect_.right, mediaRect_.bottom};
        catalogRect_.top = std::max(catalogRect_.top, mediaRect_.top + dp(115));
        divider_ = {0, catalogRect_.top - dp(4), client_.right, catalogRect_.top};
        mediaRect_.bottom = divider_.top;
    }
    const bool showDetails = fullUi && detail_ != Detail::none;
    if (showDetails) {
        detailsRect_ = contentRect;
        detailsRect_.left = std::max(mediaRect_.left + dp(110), detailsRect_.right - dp(310));
        mediaRect_.right = detailsRect_.left;
        if (panel_ == Panel::bottom) {
            catalogRect_.right = detailsRect_.left;
            divider_.right = detailsRect_.left;
        }
        const int x = detailsRect_.left + dp(16);
        const int width = std::max(98, MulDiv(spanX(detailsRect_), 96, static_cast<int>(dpi_)) - 32);
        if (detail_ == Detail::information) {
            button(Control::copy, x, detailsRect_.bottom - dp(46), 172);
        } else {
            const bool busy = exportThread_.joinable();
            button(Control::preset, x, detailsRect_.top + dp(112), width, !busy);
            button(Control::advanced, x, detailsRect_.top + dp(239), width);
            if (busy) {
                button(Control::cancel, x, detailsRect_.bottom - dp(47), width);
            } else {
                button(Control::startExport, x, detailsRect_.bottom - dp(47), width, playback_.ready);
            }
        }
    }
    const bool rangeVisible = showDetails && detail_ == Detail::exportGif;
    if (rangeVisible) {
        const int x = detailsRect_.left + dp(16);
        const int y = detailsRect_.top + dp(185);
        const int width = (spanX(detailsRect_) - dp(78)) / 2;
        SetWindowPos(rangeStart_, nullptr, x, y, width, dp(28), SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(rangeEnd_, nullptr, x + width + dp(10), y, width, dp(28), SWP_NOZORDER | SWP_NOACTIVATE);
        button(Control::timestamp, detailsRect_.right - dp(42), y - dp(2), 28,
               playback_.ready && !exportThread_.joinable());
        SendMessageW(rangeStart_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(dp(5), dp(5)));
        SendMessageW(rangeEnd_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(dp(5), dp(5)));
    }
    ShowWindow(rangeStart_, rangeVisible ? SW_SHOWNA : SW_HIDE);
    ShowWindow(rangeEnd_, rangeVisible ? SW_SHOWNA : SW_HIDE);
    EnableWindow(rangeStart_, !exportThread_.joinable());
    EnableWindow(rangeEnd_, !exportThread_.joinable());

    const bool showVolume = moving && kind_ == MediaKind::video;
    if (playbackBar && moving) {
        const int bottom = playbackRect_.bottom;
        const int width = spanX(playbackRect_);
        const int left = playbackRect_.left;
        const int y = playbackRect_.top + dp(21);
        const int right = playbackRect_.right - dp(9);
        int x = left + dp(9);
        const bool navigateButtons = fullUi && width >= dp(720);
        if (navigateButtons) { button(Control::previous, x, y, 32, current_ > 0); x += dp(35); }
        button(Control::back, x, y, 32);
        button(Control::play, x + dp(36), y, 36);
        button(Control::step, x + dp(76), y, 32);
        if (navigateButtons) {
            button(Control::next, x + dp(111), y, 32,
                   current_ >= 0 && current_ + 1 < static_cast<int>(files_.size()));
        }
        int tools = right;
        if (!quickUi) {
            tools -= dp(33);
            button(Control::fullScreen, tools, y, 33);
            tools -= dp(5);
        }
        if (showVolume) {
            const int sliderWidth = quickUi ? (width < dp(455) ? 54 : 76) : 100;
            tools -= dp(sliderWidth);
            SetWindowPos(volumeTrack_, HWND_TOP, tools, y + dp(3), dp(sliderWidth), dp(26),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            tools -= dp(31);
            button(Control::volume, tools, y, 29);
            tools -= dp(5);
        }
        tools -= dp(46);
        button(Control::speed, tools, y, 44);
        tools -= dp(34);
        button(Control::capture, tools, y, 32, playback_.ready);
        if (fullUi && width >= dp(670)) {
            tools -= dp(35);
            button(Control::zoom, tools, y, 32);
        }
        const int counterWidth = 155;
        if (narrowQuick) {
            button(Control::counter, left + dp(10), y + dp(31), counterWidth);
            filenameRect_ = {left + dp(counterWidth + 19), y + dp(31), right, bottom};
        } else {
            const int counter = left + dp(navigateButtons ? 193 : 116);
            button(Control::counter, counter, y, counterWidth);
            filenameRect_ = {counter + dp(counterWidth + 3), y, tools - dp(7), bottom - dp(4)};
        }
        timeline_ = {left + dp(12), playbackRect_.top + dp(3), right - dp(3), playbackRect_.top + dp(18)};
    } else if (playbackBar && quickUi) {
        filenameRect_ = {dp(11), playbackRect_.top, playbackRect_.right - dp(11), playbackRect_.bottom};
    } else if (fullUi && kind_ == MediaKind::image) {
        filenameRect_ = {dp(150), statusRect_.top + 1, statusRect_.right - dp(106), statusRect_.bottom};
    }
    if (!IsRectEmpty(&filenameRect_) && !path_.empty()) {
        HDC dc = GetDC(window_);
        if (dc) {
            const auto oldFont = SelectObject(dc, font_);
            const auto name = path_.filename().wstring();
            SIZE size{};
            GetTextExtentPoint32W(dc, name.c_str(), static_cast<int>(name.size()), &size);
            filenameOverflows_ = size.cx > spanX(filenameRect_);
            SelectObject(dc, oldFont);
            ReleaseDC(window_, dc);
        }
    } else {
        filenameOverflows_ = false;
    }
    if (filenameOverflows_) SetTimer(window_, filenameTimer, 40, nullptr);
    else KillTimer(window_, filenameTimer);
    if (!showVolume) ShowWindow(volumeTrack_, SW_HIDE);

    const auto showStrip = [&](HWND strip, RECT rect) {
        if (!strip) return;
        if (IsRectEmpty(&rect)) {
            ShowWindow(strip, SW_HIDE);
        } else {
            SetWindowPos(strip, HWND_TOP, rect.left, rect.top, spanX(rect), spanY(rect),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(strip, nullptr, FALSE);
        }
    };
    showStrip(controlsStrip_, playbackRect_);
    if (showVolume) SetWindowPos(volumeTrack_, HWND_TOP, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        if (!visible[index]) ShowWindow(buttons_[index], SW_HIDE);
        else SetWindowPos(buttons_[index], HWND_TOP, 0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    const bool showMedia = player_ && kind_ && !playback_.failed && spanX(mediaRect_) > 0 && spanY(mediaRect_) > 0;
    SetWindowPos(video_, HWND_BOTTOM, mediaRect_.left, mediaRect_.top, std::max(0, spanX(mediaRect_)),
                 std::max(0, spanY(mediaRect_)), SWP_NOACTIVATE |
                 (showMedia ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    scroll_ = std::clamp(scroll_, 0, maxScroll());
    requestThumbnails();
    updateCounter();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::paintButton(const DRAWITEMSTRUCT& item)
{
    const Control id = static_cast<Control>(item.CtlID);
    const bool disabled = item.itemState & ODS_DISABLED;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hover = item.itemState & ODS_HOTLIGHT;
    const bool active = (id == Control::catalog && panel_ != Panel::hidden) ||
                        (id == Control::info && detail_ == Detail::information) ||
                        (id == Control::exportGif && detail_ == Detail::exportGif) ||
                        (id == Control::pin && onTop_);
    const bool primary = id == Control::play || id == Control::startExport;
    const bool card = id == Control::preset || id == Control::advanced || id == Control::copy ||
                      id == Control::cancel || id == Control::timestamp || primary;
    const auto icon = id == Control::volume && playback_.volume <= 0
        ? std::wstring_view(L"\xE74F") : iconFor(id, playback_.paused, fullscreen_, panel_ == Panel::bottom);
    const bool onStrip = id == Control::previous || id == Control::back || id == Control::play ||
                         id == Control::step || id == Control::next || id == Control::speed ||
                         id == Control::volume || id == Control::zoom || id == Control::fullScreen ||
                         id == Control::capture || id == Control::counter;
    const COLORREF base = onStrip || (card && id != Control::play) ? surface : background;
    const COLORREF face = disabled ? base : primary ? (pressed || hover ? accentHover : accent)
        : active ? activeSurface : card || pressed || hover ? raised : base;
    fill(item.hDC, item.rcItem, base);
    rounded(item.hDC, item.rcItem, face, card && !primary ? border : face, dp(11));
    const auto oldFont = SelectObject(item.hDC, icon.empty() ? font_ : iconFont_);
    wchar_t value[100]{};
    if (icon.empty()) GetWindowTextW(item.hwndItem, value, static_cast<int>(std::size(value)));
    const bool counter = id == Control::counter;
    RECT caption = item.rcItem;
    if (counter) caption.left += dp(4);
    label(item.hDC, icon.empty() ? std::wstring_view(value) : icon, caption,
          disabled ? hint : primary ? background : active ? accentHover : textColor,
          (counter ? DT_LEFT : DT_CENTER) | DT_SINGLELINE | DT_VCENTER | (counter ? 0 : DT_END_ELLIPSIS));
    SelectObject(item.hDC, oldFont);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT App::paintVolume(const NMCUSTOMDRAW& draw)
{
    if (draw.dwDrawStage == CDDS_PREPAINT) {
        RECT bounds{};
        GetClientRect(volumeTrack_, &bounds);
        fill(draw.hdc, bounds, surface);
        RECT channel{}, thumb{};
        SendMessageW(volumeTrack_, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(volumeTrack_, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const int position = (thumb.left + thumb.right) / 2;
        const int middle = (bounds.top + bounds.bottom) / 2;
        rounded(draw.hdc, {channel.left, middle - dp(2), channel.right, middle + dp(2)}, border, border, dp(4));
        rounded(draw.hdc, {channel.left, middle - dp(2), position + dp(1), middle + dp(2)}, accent, accent, dp(4));
        rounded(draw.hdc, {position - dp(5), middle - dp(5), position + dp(5), middle + dp(5)},
                accentHover, accentHover, dp(10));
        if (GetFocus() == volumeTrack_) DrawFocusRect(draw.hdc, &bounds);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

void App::paintCatalog(HDC dc)
{
    if (IsRectEmpty(&catalogRect_)) return;
    fill(dc, catalogRect_, surface);
    const RECT title{catalogRect_.left + dp(11), catalogRect_.top,
                     catalogRect_.right - dp(10), catalogRect_.top + dp(30)};
    label(dc, L"Каталог", title, textColor);
    label(dc, std::to_wstring(files_.size()) + L"   ···", title, hint,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, catalogRect_.left, title.bottom, catalogRect_.right, catalogRect_.bottom);
    const bool bottom = panel_ == Panel::bottom;
    const int stride = bottom ? strideX() : strideY();
    const int first = std::max(0, (scroll_ / stride - 1) * columns());
    const int limit = std::min(static_cast<int>(files_.size()),
        first + ((bottom ? spanX(catalogRect_) : spanY(catalogRect_)) / stride + 4) * columns());
    const HDC memory = CreateCompatibleDC(dc);
    for (int index = first; index < limit; ++index) {
        const auto& file = files_[static_cast<std::size_t>(index)];
        const RECT rect = itemRect(static_cast<std::size_t>(index));
        rounded(dc, rect, index == current_ ? activeSurface : background,
                index == current_ ? accent : border, dp(10));
        if (cards_ || bottom) {
            RECT placeholder = rect;
            InflateRect(&placeholder, -dp(5), -dp(5));
            placeholder.bottom -= dp(21);
            const auto image = thumbnails_.find(file.path);
            if (image != thumbnails_.end() && image->second) {
                BITMAP bitmap{};
                GetObjectW(image->second.get(), sizeof(bitmap), &bitmap);
                if (bitmap.bmWidth > 0 && bitmap.bmHeight > 0 && spanX(placeholder) > 0 && spanY(placeholder) > 0) {
                    const auto old = SelectObject(memory, image->second.get());
                    const double scale = std::min(static_cast<double>(spanX(placeholder)) / bitmap.bmWidth,
                                                  static_cast<double>(spanY(placeholder)) / bitmap.bmHeight);
                    const int width = std::max(1, static_cast<int>(bitmap.bmWidth * scale));
                    const int height = std::max(1, static_cast<int>(bitmap.bmHeight * scale));
                    SetStretchBltMode(dc, HALFTONE);
                    StretchBlt(dc, placeholder.left + (spanX(placeholder) - width) / 2,
                               placeholder.top + (spanY(placeholder) - height) / 2,
                               width, height, memory, 0, 0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
                    SelectObject(memory, old);
                }
            } else {
                label(dc, file.extension, placeholder, hint, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }
            label(dc, file.name, {rect.left + dp(7), rect.bottom - dp(23), rect.right - dp(4), rect.bottom});
        } else {
            label(dc, file.name, {rect.left + dp(8), rect.top, rect.right - dp(5), rect.bottom});
        }
    }
    DeleteDC(memory);
    RestoreDC(dc, saved);
}

std::wstring App::fileInfo() const
{
    if (path_.empty()) return {};
    std::wostringstream result;
    result << path_.filename().wstring() << L"\n" << path_.wstring() << L"\n\n";
    if (hasFileMetadata_) {
        const auto bytes = (static_cast<std::uint64_t>(fileMetadata_.nFileSizeHigh) << 32) | fileMetadata_.nFileSizeLow;
        result << L"Размер   " << decimal(static_cast<double>(bytes) / (1024 * 1024), 2)
               << L" МиБ (" << bytes << L" байт)\n"
               << L"Создан   " << localDate(fileMetadata_.ftCreationTime) << L"\n"
               << L"Изменён   " << localDate(fileMetadata_.ftLastWriteTime) << L"\n";
    }
    if (playback_.width > 0 && playback_.height > 0) result << playback_.width << L" × " << playback_.height << L" px\n";
    if (kind_ != MediaKind::image && playback_.duration > 0) result << L"Длительность   " << timeLabel(playback_.duration) << L"\n";
    if (kind_ != MediaKind::image && playback_.fps > 0) result << decimal(playback_.fps, 2) << L" FPS\n";
    if (!playback_.videoCodec.empty()) result << L"Кодек   " << playback_.videoCodec << L"\n";
    if (!playback_.audioCodec.empty()) result << L"Аудио   " << playback_.audioCodec << L"\n";
    return result.str();
}

void App::paintDetails(HDC dc)
{
    if (IsRectEmpty(&detailsRect_)) return;
    fill(dc, detailsRect_, surface);
    fill(dc, {detailsRect_.left, detailsRect_.top, detailsRect_.left + 1, detailsRect_.bottom}, border);
    const LONG x = detailsRect_.left + dp(16);
    const LONG y = detailsRect_.top;
    const LONG right = detailsRect_.right - dp(16);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, detailsRect_.left, detailsRect_.top, detailsRect_.right, detailsRect_.bottom);
    const auto oldFont = SelectObject(dc, titleFont_);
    label(dc, detail_ == Detail::information ? L"Сведения о файле" : L"Экспорт медиа",
          {x, y + dp(10), right, y + dp(47)});
    SelectObject(dc, oldFont);
    if (detail_ == Detail::information) {
        label(dc, fileInfo(), {x, y + dp(58), right, detailsRect_.bottom - dp(53)}, hint,
              DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
    } else {
        const LONG middle = x + (spanX(detailsRect_) - dp(78)) / 2 + dp(10);
        label(dc, L"Формат", {x, y + dp(51), right, y + dp(74)}, hint);
        label(dc, L"GIF · анимация", {x, y + dp(72), right, y + dp(99)});
        label(dc, L"Качество", {x, y + dp(92), right, y + dp(112)}, hint);
        label(dc, L"Фрагмент", {x, y + dp(147), right, y + dp(166)});
        label(dc, L"От", {x, y + dp(167), middle, y + dp(183)}, rangeTarget_ == rangeStart_ ? accentHover : hint);
        label(dc, L"До", {middle, y + dp(167), right, y + dp(183)}, rangeTarget_ == rangeEnd_ ? accentHover : hint);
        const double duration = readRange(rangeEnd_).value_or(playback_.duration) - readRange(rangeStart_).value_or(0);
        const std::wstring range = duration > 0
            ? L"Длительность фрагмента   " + decimal(duration, 2) + L" с"
            : L"Укажите границы фрагмента";
        label(dc, range, {x, y + dp(215), right, y + dp(236)}, hint);
        if (exportAdvanced_) {
            std::wstring_view settings = L"До 720 px  ·  15 FPS  ·  192 цвета";
            if (preset_ == GifPreset::compact) settings = L"До 480 px  ·  10 FPS  ·  96 цветов";
            if (preset_ == GifPreset::quality) settings = L"До 1080 px  ·  24 FPS  ·  256 цветов";
            label(dc, settings, {x, y + dp(275), right, y + dp(296)}, hint);
        }
        if (!status_.empty() && detailsRect_.bottom > y + dp(377)) {
            label(dc, status_, {x, y + dp(297), right, detailsRect_.bottom - dp(51)}, hint,
                  DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
        }
    }
    RestoreDC(dc, saved);
}

void App::paintFilename(HDC dc, RECT bounds)
{
    if (path_.empty() || IsRectEmpty(&bounds)) return;
    const auto stem = path_.stem().wstring();
    const auto extension = path_.extension().wstring();
    SIZE stemSize{}, extensionSize{};
    GetTextExtentPoint32W(dc, stem.c_str(), static_cast<int>(stem.size()), &stemSize);
    GetTextExtentPoint32W(dc, extension.c_str(), static_cast<int>(extension.size()), &extensionSize);
    const int width = spanX(bounds);
    const int combined = stemSize.cx + extensionSize.cx;
    const int start = bounds.left + std::max(0, (width - combined) / 2);
    const int suffix = std::max(static_cast<int>(bounds.left), std::min(start + static_cast<int>(stemSize.cx),
        static_cast<int>(bounds.right - extensionSize.cx)));
    if (suffix > start) {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, start, bounds.top, suffix, bounds.bottom);
        const int overshoot = std::max(0, static_cast<int>(stemSize.cx) - (suffix - start));
        const double distance = static_cast<double>(overshoot);
        const double travelMs = distance * 1000.0 / std::max(1, dp(42));
        const double elapsed = std::fmod(static_cast<double>(GetTickCount64() - filenameSince_),
                                         1600.0 + 2.0 * travelMs);
        const double shifted = elapsed <= 800.0 + travelMs ? elapsed - 800.0 :
                               1600.0 + 2.0 * travelMs - elapsed - 800.0;
        const int offset = static_cast<int>(std::clamp(shifted * dp(42) / 1000.0, 0.0, distance));
        label(dc, stem, {start - offset, bounds.top, start - offset + stemSize.cx + 2, bounds.bottom},
              textColor, DT_SINGLELINE | DT_VCENTER);
        RestoreDC(dc, saved);
    }

    label(dc, extension, {suffix, bounds.top, bounds.right, bounds.bottom},
          hint, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void App::paintStrip()
{
    PAINTSTRUCT state{};
    HDC dc = BeginPaint(controlsStrip_, &state);
    RECT client{};
    GetClientRect(controlsStrip_, &client);
    fill(dc, client, surface);
    fill(dc, {client.left, client.top, client.right, client.top + 1}, border);
    const auto oldFont = SelectObject(dc, font_);
    if (!IsRectEmpty(&timeline_)) {
        RECT track = timeline_;
        OffsetRect(&track, -playbackRect_.left, -playbackRect_.top);
        const int y = (track.top + track.bottom) / 2;
        rounded(dc, {track.left, y - dp(2), track.right, y + dp(2)}, border, border, dp(4));
        if (playback_.duration > 0.0) {
            const auto fraction = std::clamp(playback_.position / playback_.duration, 0.0, 1.0);
            const int progress = track.left + static_cast<int>(spanX(track) * fraction);
            rounded(dc, {track.left, y - dp(2), progress + dp(2), y + dp(2)}, accent, accent, dp(4));
            rounded(dc, {progress - dp(4), y - dp(4), progress + dp(4), y + dp(4)}, accent, accent, dp(8));
        }
    }
    RECT filename = filenameRect_;
    OffsetRect(&filename, -playbackRect_.left, -playbackRect_.top);
    paintFilename(dc, filename);
    SelectObject(dc, oldFont);
    EndPaint(controlsStrip_, &state);
}

void App::paint()
{
    PAINTSTRUCT state{};
    HDC screen = BeginPaint(window_, &state);
    if (IsRectEmpty(&client_) || IsRectEmpty(&state.rcPaint)) {
        EndPaint(window_, &state);
        return;
    }
    HDC dc = CreateCompatibleDC(screen);
    const RECT update = state.rcPaint;
    HBITMAP frame = CreateCompatibleBitmap(screen, spanX(update), spanY(update));
    if (!dc || !frame) {
        if (dc) DeleteDC(dc);
        if (frame) DeleteObject(frame);
        EndPaint(window_, &state);
        return;
    }
    const auto old = SelectObject(dc, frame);
    const auto oldFont = SelectObject(dc, font_);
    SetViewportOrgEx(dc, -update.left, -update.top, nullptr);
    fill(dc, update, background);
    RECT visible{};
    if (IntersectRect(&visible, &state.rcPaint, &catalogRect_)) paintCatalog(dc);
    if (IntersectRect(&visible, &state.rcPaint, &detailsRect_)) paintDetails(dc);
    if (!IsRectEmpty(&divider_)) fill(dc, divider_, border);
    if (!IsRectEmpty(&headerRect_)) {
        fill(dc, {headerRect_.left, headerRect_.bottom - 1, headerRect_.right, headerRect_.bottom}, border);
    }
    if ((!player_ || !kind_ || playback_.failed) && !IsRectEmpty(&mediaRect_)) {
        const LONG center = (mediaRect_.top + mediaRect_.bottom) / 2;
        const RECT title{mediaRect_.left + dp(15), center - dp(35), mediaRect_.right - dp(15), center};
        SelectObject(dc, titleFont_);
        label(dc, kind_ ? L"Не удалось открыть файл" : L"Откройте фото, GIF или видео", title, textColor,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, font_);
        if (!status_.empty()) label(dc, status_, {title.left, center, title.right, center + dp(90)},
                                    hint, DT_CENTER | DT_WORDBREAK);
        else label(dc, L"Перетащите сюда файл или папку", {title.left, center, title.right, center + dp(32)},
                   hint, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    if (!IsRectEmpty(&statusRect_)) {
        fill(dc, {0, statusRect_.top, statusRect_.right, statusRect_.top + 1}, border);
        std::wstring summary;
        if (playback_.width && playback_.height) {
            summary = std::to_wstring(playback_.width) + L" × " + std::to_wstring(playback_.height);
            if (!playback_.videoCodec.empty()) summary += L"   ·   " + playback_.videoCodec;
            if (playback_.fps > 0 && kind_ != MediaKind::image) summary += L"   ·   " + decimal(playback_.fps, 1) + L" FPS";
        }
        if (!status_.empty()) summary = status_;
        const auto summaryRight = IsRectEmpty(&playbackRect_) && !IsRectEmpty(&filenameRect_)
            ? filenameRect_.left - dp(8) : statusRect_.right - dp(100);
        label(dc, summary, {dp(15), statusRect_.top, summaryRight, statusRect_.bottom}, hint);
        if (IsRectEmpty(&playbackRect_)) paintFilename(dc, filenameRect_);
        if (current_ >= 0) {
            label(dc, std::to_wstring(current_ + 1) + L" / " + std::to_wstring(files_.size()),
                  {statusRect_.right - dp(96), statusRect_.top, statusRect_.right - dp(15), statusRect_.bottom},
                  hint, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
    }
    BitBlt(screen, update.left, update.top, spanX(update), spanY(update), dc, update.left, update.top, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, old);
    DeleteObject(frame);
    DeleteDC(dc);
    EndPaint(window_, &state);
}

void App::refresh()
{
    if (player_) {
        PlaybackState next = player_->poll();
        const bool readyChanged = playback_.ready != next.ready;
        const bool durationChanged = playback_.duration != next.duration;
        const bool failChanged = playback_.failed != next.failed;
        const bool pausedChanged = playback_.paused != next.paused;
        const bool speedChanged = playback_.speed != next.speed;
        const bool timeChanged = playback_.duration != next.duration ||
                                 std::abs(playback_.position - next.position) > 0.025;
        const bool infoChanged = playback_.width != next.width || playback_.height != next.height ||
                                 playback_.fps != next.fps || playback_.videoCodec != next.videoCodec ||
                                 playback_.audioCodec != next.audioCodec;
        playback_ = std::move(next);
        if (rangeEndAutomatic_ && durationChanged && kind_ == MediaKind::video && playback_.duration > 0) {
            setRangeValue(rangeEnd_, playback_.duration);
        }
        updateCounter();
        if (kind_ == MediaKind::video && volumeTrack_) {
            const int volume = static_cast<int>(std::lround(playback_.volume));
            if (SendMessageW(volumeTrack_, TBM_GETPOS, 0, 0) != volume) {
                SendMessageW(volumeTrack_, TBM_SETPOS, TRUE, volume);
            }
        }
        if (playback_.failed && !playback_.error.empty()) status_ = playback_.error;
        if (pausedChanged || speedChanged) {
            const auto set = [this](Control id, const std::wstring& value) {
                SetWindowTextW(buttons_[static_cast<UINT>(id) - static_cast<UINT>(Control::open)], value.c_str());
            };
            if (pausedChanged) {
                set(Control::play, playback_.paused ? L"Воспроизвести" : L"Пауза");
                InvalidateRect(buttons_[static_cast<UINT>(Control::play) - static_cast<UINT>(Control::open)], nullptr, FALSE);
            }
            if (speedChanged) set(Control::speed, decimal(playback_.speed, 2) + L"×");
        }
        if (failChanged || readyChanged) layout();
        if (timeChanged && !IsRectEmpty(&timeline_)) {
            InvalidateRect(controlsStrip_, nullptr, FALSE);
        }
        if (infoChanged && detail_ == Detail::information) InvalidateRect(window_, &detailsRect_, FALSE);
        if (infoChanged && !IsRectEmpty(&statusRect_)) InvalidateRect(window_, &statusRect_, FALSE);
        if (quickFitPending_ && mode_ == Mode::quick && !fullscreen_ &&
            playback_.width && playback_.height) fitQuickSize();
        if (auto frame = player_->takeFrame()) acceptFrame(std::move(*frame));
    }
    refreshQuickZOrder();
}

void App::updateCounter()
{
    const HWND counter = buttons_[static_cast<UINT>(Control::counter) - static_cast<UINT>(Control::open)];
    if (!counter || !kind_ || kind_ == MediaKind::image) return;
    std::wstring value;
    if (!showFrames_) {
        value = timeLabel(playback_.position) + L" / " + timeLabel(playback_.duration > 0 ? playback_.duration : -1);
    } else if (std::isfinite(playback_.fps) && playback_.fps > 0 && std::isfinite(playback_.duration) &&
               std::isfinite(playback_.position) && playback_.duration > 0 &&
               playback_.duration * playback_.fps < 1e12) {
        const auto count = std::max(1LL, std::llround(playback_.duration * playback_.fps));
        const auto current = std::clamp(std::llround(std::clamp(playback_.position, 0.0, playback_.duration) *
                                                     playback_.fps) + 1, 1LL, count);
        value = L"≈ " + std::to_wstring(current) + L" / " + std::to_wstring(count) + L" кад.";
    } else {
        value = L"— / — кад.";
    }
    wchar_t old[64]{};
    GetWindowTextW(counter, old, static_cast<int>(std::size(old)));
    if (value != old) SetWindowTextW(counter, value.c_str());
}

void App::captureFrame(FrameCapturePurpose purpose)
{
    if (!player_ || !playback_.ready || !capturePath_.empty()) return;
    if (player_->requestFrame(purpose)) capturePath_ = path_;
}

void App::acceptFrame(FrameCaptureResult result)
{
    auto capturedPath = std::exchange(capturePath_, {});
    if (capturedPath.empty() || !IsWindowVisible(window_)) return;
    if (!result.frame) {
        MessageBoxW(window_, L"Не удалось получить текущий стоп-кадр.", L"Стоп-кадр", MB_OK | MB_ICONWARNING);
        return;
    }
    if (result.purpose == FrameCapturePurpose::clipboard) {
        if (!copyFrameToClipboard(window_, *result.frame)) {
            MessageBoxW(window_, L"Не удалось скопировать стоп-кадр.", L"Стоп-кадр", MB_OK | MB_ICONWARNING);
        }
        return;
    }
    const auto suggested = capturedPath.parent_path() / (capturedPath.stem().wstring() + L"-frame.png");
    auto output = dialogPath(window_, false, &suggested, true);
    if (!output) return;
    if (CompareStringOrdinal(output->extension().c_str(), -1, L".png", -1, TRUE) != CSTR_EQUAL ||
        samePath(*output, capturedPath) || !saveFramePng(*output, *result.frame, true)) {
        MessageBoxW(window_, L"Не удалось сохранить стоп-кадр. Выберите отдельный файл с расширением .png.",
                    L"Стоп-кадр", MB_OK | MB_ICONWARNING);
    }
}

void App::changeFullscreen()
{
    if (!fullscreen_) {
        placement_.length = sizeof(placement_);
        GetWindowPlacement(window_, &placement_);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        fullscreen_ = true;
        SetWindowLongPtrW(window_, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE);
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     spanX(monitor.rcMonitor), spanY(monitor.rcMonitor), SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        fullscreen_ = false;
        const LONG_PTR style = WS_CLIPCHILDREN | WS_VISIBLE | (mode_ == Mode::full ? WS_OVERLAPPEDWINDOW
                                                                                  : WS_POPUP | WS_THICKFRAME);
        SetWindowLongPtrW(window_, GWL_STYLE, style);
        SetWindowPlacement(window_, &placement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    cancelCanvasClick();
    layout();
}

RECT App::quickBounds() const
{
    POINT pointer{};
    GetCursorPos(&pointer);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT intersection{};
    if (hasQuickSize_ && IntersectRect(&intersection, &quickWindow_, &monitor.rcWork) &&
        spanX(intersection) >= dp(220) && spanY(intersection) >= dp(130)) {
        return quickWindow_;
    }
    const int width = std::min(dp(710), spanX(monitor.rcWork) - dp(24));
    const int height = std::min(dp(450), spanY(monitor.rcWork) - dp(24));
    const LONG left = monitor.rcWork.left + (spanX(monitor.rcWork) - width) / 2;
    const LONG top = monitor.rcWork.top + (spanY(monitor.rcWork) - height) / 2;
    return {left, top, left + width, top + height};
}

void App::fitQuickSize()
{
    quickFitPending_ = false;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const double ratio = static_cast<double>(playback_.width) / playback_.height;
    const int maxWidth = std::min(dp(800), spanX(monitor.rcWork) * 4 / 5);
    const int maxHeight = std::min(dp(560), spanY(monitor.rcWork) * 3 / 4);
    const int width = std::clamp(static_cast<int>(maxHeight * ratio), std::min(dp(365), maxWidth), maxWidth);
    const int height = std::clamp(static_cast<int>(width / ratio) + dp(kind_ == MediaKind::image ? 36 : 57),
                                  std::min(dp(300), maxHeight), maxHeight);
    SetWindowPos(window_, nullptr, monitor.rcWork.left + (spanX(monitor.rcWork) - width) / 2,
                 monitor.rcWork.top + (spanY(monitor.rcWork) - height) / 2, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

void App::saveQuickSize()
{
    if (IsWindowVisible(window_) && !IsIconic(window_) && GetWindowRect(window_, &quickWindow_)) {
        hasQuickSize_ = true;
        savePreferences();
    }
}

void App::togglePin()
{
    onTop_ = !onTop_;
    refreshQuickZOrder();
    savePreferences();
}

void App::refreshQuickZOrder()
{
    if (mode_ != Mode::quick || !IsWindowVisible(window_)) return;
    const HWND source = explorer_->source_explorer();
    const HWND foreground = GetForegroundWindow();
    const bool shouldStayAbove = onTop_ || (source && (foreground == source || foreground == window_));
    const bool topmost = (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (shouldStayAbove != topmost) {
        SetWindowPos(window_, shouldStayAbove ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void App::click(POINT point, UINT buttons)
{
    if (PtInRect(&timeline_, point) && playback_.duration > 0 && player_) {
        SetCapture(window_);
        scrubbing_ = true;
        player_->seekAbsolute(playback_.duration * (point.x - timeline_.left) / spanX(timeline_));
    } else if (PtInRect(&divider_, point)) {
        SetCapture(window_);
        resizingPanel_ = true;
    } else if (PtInRect(&catalogRect_, point)) {
        if (point.y < catalogRect_.top + dp(30)) {
            ClientToScreen(window_, &point);
            catalogMenu(point);
            return;
        }
        const bool bottom = panel_ == Panel::bottom;
        const int index = bottom
            ? (point.x - catalogRect_.left - dp(10) + scroll_) / strideX()
            : ((point.y - catalogRect_.top - dp(31) + scroll_) / strideY()) * columns() +
              (point.x - catalogRect_.left - dp(10)) / strideX();
        if (index >= 0 && index < static_cast<int>(files_.size())) {
            const RECT bounds = itemRect(static_cast<std::size_t>(index));
            if (PtInRect(&bounds, point)) {
                const auto file = files_[static_cast<std::size_t>(index)].path;
                openMedia(file);
            }
        }
    } else if (PtInRect(&mediaRect_, point)) beginCanvasGesture(point, (buttons & MK_CONTROL) != 0);
}

void App::cancelCanvasClick()
{
    KillTimer(window_, canvasClickTimer);
    pendingClickTime_ = 0;
}

void App::beginCanvasGesture(POINT point, bool ctrl)
{
    ClientToScreen(window_, &point);
    const auto within = [](LONG delta, int dimension) { return std::abs(delta) <= std::max(1, dimension / 2); };
    doubleClick_ = !ctrl && pendingClickTime_ &&
        GetTickCount64() - pendingClickTime_ <= GetDoubleClickTime() &&
        within(point.x - pendingClickPoint_.x, GetSystemMetricsForDpi(SM_CXDOUBLECLK, dpi_)) &&
        within(point.y - pendingClickPoint_.y, GetSystemMetricsForDpi(SM_CYDOUBLECLK, dpi_));
    if (pendingClickTime_ && !doubleClick_ && player_) player_->togglePause();
    cancelCanvasClick();
    gestureCtrl_ = ctrl;
    gestureOrigin_ = gestureLast_ = point;
    GetWindowRect(window_, &gestureWindow_);
    gesture_ = CanvasGesture::pressed;
    SetCapture(window_);
}

void App::moveCanvasGesture(POINT point)
{
    ClientToScreen(window_, &point);
    if (gesture_ == CanvasGesture::pressed) {
        if (std::abs(point.x - gestureOrigin_.x) < GetSystemMetricsForDpi(SM_CXDRAG, dpi_) &&
            std::abs(point.y - gestureOrigin_.y) < GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) return;
        gesture_ = gestureCtrl_ ? CanvasGesture::pan : CanvasGesture::moveWindow;
        doubleClick_ = false;
        if (gesture_ == CanvasGesture::moveWindow && !fullscreen_ && IsZoomed(window_)) {
            const double anchor = std::clamp(static_cast<double>(gestureOrigin_.x - gestureWindow_.left) /
                                             std::max(1, spanX(gestureWindow_)), 0.0, 1.0);
            ShowWindow(window_, SW_RESTORE);
            GetWindowRect(window_, &gestureWindow_);
            gestureWindow_.left = gestureOrigin_.x - static_cast<LONG>(anchor * spanX(gestureWindow_));
            gestureWindow_.top = gestureOrigin_.y - dp(18);
        }
    }
    if (gesture_ == CanvasGesture::pan && player_) {
        player_->pan(static_cast<double>(point.x - gestureLast_.x) / std::max(1, spanX(mediaRect_)),
                     static_cast<double>(point.y - gestureLast_.y) / std::max(1, spanY(mediaRect_)));
    } else if (gesture_ == CanvasGesture::moveWindow && !fullscreen_) {
        SetWindowPos(window_, nullptr, gestureWindow_.left + point.x - gestureOrigin_.x,
                     gestureWindow_.top + point.y - gestureOrigin_.y, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
    gestureLast_ = point;
}

void App::endCanvasGesture(POINT point)
{
    if (gesture_ == CanvasGesture::pressed) moveCanvasGesture(point);
    const bool isClick = gesture_ == CanvasGesture::pressed && !gestureCtrl_;
    const bool doubleClick = isClick && doubleClick_;
    gesture_ = CanvasGesture::none;
    doubleClick_ = false;
    if (GetCapture() == window_) ReleaseCapture();
    if (doubleClick) {
        if (mode_ == Mode::quick) promotePreview();
        else changeFullscreen();
    } else if (isClick && kind_) {
        ClientToScreen(window_, &point);
        pendingClickPoint_ = point;
        pendingClickTime_ = GetTickCount64();
        if (!SetTimer(window_, canvasClickTimer, GetDoubleClickTime(), nullptr)) {
            cancelCanvasClick();
            if (player_ && kind_ != MediaKind::image) player_->togglePause();
        }
    }
}

void App::setVolume(double value)
{
    if (!player_ || kind_ != MediaKind::video || !playback_.ready) return;
    if (playback_.volume > 0) lastAudibleVolume_ = playback_.volume;
    player_->changeVolume(std::clamp(value, 0.0, 100.0) - playback_.volume);
    playback_.volume = player_->poll().volume;
    if (playback_.volume > 0) lastAudibleVolume_ = playback_.volume;
    SendMessageW(volumeTrack_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(playback_.volume)));
    InvalidateRect(buttons_[static_cast<UINT>(Control::volume) - static_cast<UINT>(Control::open)], nullptr, FALSE);
}

bool App::key(WPARAM code, bool alt)
{
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (code == VK_F4 && alt) return false;
    if (code == VK_ESCAPE) {
        if (fullscreen_) changeFullscreen();
        else if (mode_ == Mode::quick) hideViewer();
        else if (detail_ != Detail::none) { detail_ = Detail::none; layout(); }
        return true;
    }
    if (code == VK_F11) { changeFullscreen(); return true; }
    if (code == VK_F4 && mode_ == Mode::full && !fullscreen_) {
        command(static_cast<UINT>(Control::catalog));
        return true;
    }
    if (ctrl && code == 'O') { choose(shift); return true; }
    if (code == 'I' && !alt && !ctrl) { command(static_cast<UINT>(Control::info)); return true; }
    if (code == '0' && !alt && player_) { player_->resetView(); return true; }
    if (code == VK_LEFT || code == VK_RIGHT) {
        const int direction = code == VK_LEFT ? -1 : 1;
        if (alt) navigate(direction);
        else if (player_ && kind_ == MediaKind::image) player_->pan(direction * 0.08, 0.0);
        else if (player_ && shift) player_->seekRelative(direction * 5.0);
        else if (player_ && !ctrl) player_->stepFrame(direction);
        return true;
    }
    if (!player_ || alt || ctrl) return false;
    if (code == VK_SPACE && kind_ != MediaKind::image) { player_->togglePause(); return true; }
    if (code == VK_OEM_COMMA || code == VK_OEM_PERIOD) {
        if (kind_ == MediaKind::image) return false;
        player_->stepFrame(code == VK_OEM_COMMA ? -1 : 1);
        return true;
    }
    if (code == VK_UP || code == VK_DOWN) {
        const int direction = code == VK_UP ? 1 : -1;
        if (kind_ == MediaKind::image) player_->pan(0.0, -direction * 0.08);
        else setVolume(playback_.volume + direction * 5.0);
        return true;
    }
    if (code == VK_OEM_PLUS || code == VK_ADD) { player_->changeZoom(0.16); return true; }
    if (code == VK_OEM_MINUS || code == VK_SUBTRACT) { player_->changeZoom(-0.16); return true; }
    return false;
}

void App::wheel(WPARAM wparam, LPARAM lparam)
{
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(window_, &point);
    const double steps = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
    const UINT keys = GET_KEYSTATE_WPARAM(wparam);
    if (kind_ == MediaKind::video) {
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const HWND icon = buttons_[static_cast<UINT>(Control::volume) - static_cast<UINT>(Control::open)];
        for (HWND target : {icon, volumeTrack_}) {
            RECT rect{};
            if (IsWindowVisible(target) && GetWindowRect(target, &rect) && PtInRect(&rect, screen)) {
                setVolume(playback_.volume + steps * 5.0);
                return;
            }
        }
    }
    if (PtInRect(&catalogRect_, point)) {
        if (keys & MK_CONTROL) {
            thumb_ = std::clamp(thumb_ + static_cast<int>(steps * 12), 86, 178);
            savePreferences();
            layout();
            revealSelection();
        } else scrollCatalog(-static_cast<int>(steps * dp(88)));
    } else if (player_) {
        if (keys & MK_SHIFT && kind_ != MediaKind::image) player_->seekRelative(steps * 5.0);
        else if (keys & MK_CONTROL || kind_ != MediaKind::video) player_->changeZoom(steps * 0.16);
        else setVolume(playback_.volume + steps * 5.0);
    }
}

void App::changeShortcut(PreviewHotkey shortcut)
{
    if (!explorer_->set_hotkey(shortcut)) {
        MessageBoxW(window_ ? window_ : controller_,
                    L"Не удалось назначить сочетание Quick View. Возможно, оно занято другим приложением.",
                    L"Viewery", MB_OK | MB_ICONWARNING);
    }
}

void App::publishTray(bool add)
{
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = controller_;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = trayMessage;
    icon.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
    wcscpy_s(icon.szTip, L"Viewery · Предпросмотр в Проводнике");
    if (add && Shell_NotifyIconW(NIM_ADD, &icon)) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
    } else if (!add) {
        Shell_NotifyIconW(NIM_DELETE, &icon);
    }
}

void App::openMenu(POINT point, bool tray)
{
    HMENU menu = CreatePopupMenu();
    if (!tray && mode_ == Mode::quick) {
        menuItem(menu, static_cast<UINT>(Control::promote), L"Открыть в полном окне");
        menuItem(menu, static_cast<UINT>(Control::fullScreen),
                 fullscreen_ ? L"Выйти из полного экрана\tEsc" : L"Развернуть на весь экран\tF11");
        menuItem(menu, static_cast<UINT>(Control::pin), L"Закрепить поверх окон", onTop_);
        menuItem(menu, resetZoom, L"Вписать в окно\t0");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        menuItem(menu, static_cast<UINT>(Control::close), L"Закрыть предпросмотр\tEsc");
        const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                              point.x, point.y, 0, window_, nullptr));
        DestroyMenu(menu);
        if (picked) command(picked);
        return;
    }
    menuItem(menu, openFile, L"Открыть файл…\tCtrl+O");
    menuItem(menu, openFolder, L"Открыть папку…\tCtrl+Shift+O");
    if (!tray) {
        menuItem(menu, static_cast<UINT>(Control::fullScreen), L"Полный экран\tF11");
        menuItem(menu, resetZoom, L"Вписать в окно\t0");
        menuItem(menu, static_cast<UINT>(Control::previous), L"Предыдущий файл\tAlt+←");
        menuItem(menu, static_cast<UINT>(Control::next), L"Следующий файл\tAlt+→");
    }
    HMENU hotkey = CreatePopupMenu();
    const auto selected = explorer_->hotkey();
    menuItem(hotkey, hotkeySpace, L"Space", selected == PreviewHotkey::space);
    menuItem(hotkey, hotkeyCtrl, L"Ctrl+Space", selected == PreviewHotkey::ctrl_space);
    menuItem(hotkey, hotkeyCtrlShift, L"Ctrl+Shift+Space", selected == PreviewHotkey::ctrl_shift_space);
    menuItem(hotkey, hotkeyOff, L"Отключено", selected == PreviewHotkey::disabled);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hotkey), L"Сочетание Quick View");
    menuItem(menu, quit, L"Выйти из Viewery");
    const HWND owner = tray ? controller_ : window_;
    SetForegroundWindow(owner);
    const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                          point.x, point.y, 0, owner, nullptr));
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (picked) command(picked);
}

void App::catalogMenu(POINT point)
{
    HMENU menu = CreatePopupMenu();
    menuItem(menu, panelLeft, L"Панель слева", panel_ == Panel::left);
    menuItem(menu, panelBottom, L"Лента снизу", panel_ == Panel::bottom);
    menuItem(menu, panelHidden, L"Скрыть каталог");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    menuItem(menu, listView, L"Список", !cards_);
    menuItem(menu, cardsView, L"Миниатюры", cards_);
    HMENU size = CreatePopupMenu();
    menuItem(size, smallThumb, L"Малые");
    menuItem(size, mediumThumb, L"Средние");
    menuItem(size, largeThumb, L"Крупные");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(size), L"Размер миниатюр");
    HMENU order = CreatePopupMenu();
    menuItem(order, nameSort, L"По имени", sort_ == CatalogSort::name);
    menuItem(order, dateSort, L"Сначала новые", sort_ == CatalogSort::newest);
    menuItem(order, sizeSort, L"Сначала крупные", sort_ == CatalogSort::size);
    menuItem(order, typeSort, L"По типу", sort_ == CatalogSort::type);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(order), L"Сортировка");
    const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                          point.x, point.y, 0, window_, nullptr));
    DestroyMenu(menu);
    if (picked) command(picked);
}

void App::speedMenu(POINT point)
{
    HMENU menu = CreatePopupMenu();
    menuItem(menu, speedSlow, L"0.5×");
    menuItem(menu, speedNormal, L"1×");
    menuItem(menu, speedFast, L"1.5×");
    menuItem(menu, speedDouble, L"2×");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    menuItem(menu, seekBack, L"Назад на 5 секунд\tShift+←");
    menuItem(menu, seekForward, L"Вперёд на 5 секунд\tShift+→");
    const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                          point.x, point.y, 0, window_, nullptr));
    DestroyMenu(menu);
    if (picked) command(picked);
}

void App::presetMenu(POINT point)
{
    HMENU menu = CreatePopupMenu();
    const auto index = [](Control id) { return static_cast<UINT>(id) - static_cast<UINT>(Control::open); };
    menuItem(menu, static_cast<UINT>(Control::compact), buttonLabels[index(Control::compact)], preset_ == GifPreset::compact);
    menuItem(menu, static_cast<UINT>(Control::balanced), buttonLabels[index(Control::balanced)], preset_ == GifPreset::balanced);
    menuItem(menu, static_cast<UINT>(Control::quality), buttonLabels[index(Control::quality)], preset_ == GifPreset::quality);
    const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                          point.x, point.y, 0, window_, nullptr));
    DestroyMenu(menu);
    if (picked) command(picked);
}

void App::command(UINT id)
{
    const auto controlPoint = [&](Control control) {
        RECT rect{};
        GetWindowRect(buttons_[static_cast<UINT>(control) - static_cast<UINT>(Control::open)], &rect);
        return POINT{rect.left, rect.bottom + 2};
    };
    switch (id) {
    case static_cast<UINT>(Control::open): {
        const POINT point = controlPoint(Control::open);
        HMENU menu = CreatePopupMenu();
        menuItem(menu, openFile, L"Открыть файл…\tCtrl+O");
        menuItem(menu, openFolder, L"Открыть папку…\tCtrl+Shift+O");
        const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                              point.x, point.y, 0, window_, nullptr));
        DestroyMenu(menu);
        if (picked) command(picked);
        return;
    }
    case openFile: choose(false); return;
    case openFolder: choose(true); return;
    case static_cast<UINT>(Control::catalog):
        if (panel_ == Panel::hidden) panel_ = lastPanel_;
        else { lastPanel_ = panel_; panel_ = Panel::hidden; }
        break;
    case static_cast<UINT>(Control::panelPosition):
        panel_ = (panel_ == Panel::hidden ? lastPanel_ : panel_) == Panel::left ? Panel::bottom : Panel::left;
        scroll_ = 0;
        break;
    case panelLeft: panel_ = Panel::left; scroll_ = 0; break;
    case panelBottom: panel_ = Panel::bottom; scroll_ = 0; break;
    case panelHidden: lastPanel_ = panel_; panel_ = Panel::hidden; break;
    case listView: cards_ = false; if (panel_ == Panel::bottom) panel_ = Panel::left; scroll_ = 0; break;
    case cardsView: cards_ = true; scroll_ = 0; break;
    case smallThumb: thumb_ = 90; break;
    case mediumThumb: thumb_ = 124; break;
    case largeThumb: thumb_ = 170; break;
    case nameSort: sort_ = CatalogSort::name; break;
    case dateSort: sort_ = CatalogSort::newest; break;
    case sizeSort: sort_ = CatalogSort::size; break;
    case typeSort: sort_ = CatalogSort::type; break;
    case static_cast<UINT>(Control::info):
        if (kind_) detail_ = detail_ == Detail::information ? Detail::none : Detail::information;
        layout();
        return;
    case static_cast<UINT>(Control::exportGif):
        if (kind_ == MediaKind::video || exportThread_.joinable())
            detail_ = detail_ == Detail::exportGif ? Detail::none : Detail::exportGif;
        layout();
        return;
    case static_cast<UINT>(Control::more): openMenu(controlPoint(Control::more), false); return;
    case static_cast<UINT>(Control::speed): speedMenu(controlPoint(Control::speed)); return;
    case static_cast<UINT>(Control::volume):
        setVolume(playback_.volume > 0 ? 0 : lastAudibleVolume_);
        return;
    case static_cast<UINT>(Control::preset): presetMenu(controlPoint(Control::preset)); return;
    case static_cast<UINT>(Control::previous): navigate(-1); return;
    case static_cast<UINT>(Control::next): navigate(1); return;
    case static_cast<UINT>(Control::back): if (player_) player_->stepFrame(-1); return;
    case static_cast<UINT>(Control::step): if (player_) player_->stepFrame(1); return;
    case static_cast<UINT>(Control::play): if (player_) player_->togglePause(); return;
    case resetZoom:
    case static_cast<UINT>(Control::zoom): if (player_) player_->resetView(); return;
    case static_cast<UINT>(Control::fullScreen): changeFullscreen(); return;
    case static_cast<UINT>(Control::promote): promotePreview(); return;
    case static_cast<UINT>(Control::pin): togglePin(); return;
    case static_cast<UINT>(Control::close): hideViewer(); return;
    case static_cast<UINT>(Control::copy): copyText(window_, fileInfo()); return;
    case static_cast<UINT>(Control::capture): captureFrame(FrameCapturePurpose::clipboard); return;
    case static_cast<UINT>(Control::counter): showFrames_ = !showFrames_; updateCounter(); return;
    case static_cast<UINT>(Control::timestamp): useTimelinePosition(); return;
    case static_cast<UINT>(Control::compact):
    case static_cast<UINT>(Control::balanced):
    case static_cast<UINT>(Control::quality): {
        preset_ = id == static_cast<UINT>(Control::compact) ? GifPreset::compact
            : id == static_cast<UINT>(Control::quality) ? GifPreset::quality : GifPreset::balanced;
        const auto target = static_cast<UINT>(Control::preset) - static_cast<UINT>(Control::open);
        SetWindowTextW(buttons_[target], buttonLabels[id - static_cast<UINT>(Control::open)]);
        InvalidateRect(window_, &detailsRect_, FALSE);
        return;
    }
    case static_cast<UINT>(Control::advanced): exportAdvanced_ = !exportAdvanced_; layout(); return;
    case static_cast<UINT>(Control::startExport): startExport(preset_); return;
    case static_cast<UINT>(Control::cancel): exportThread_.request_stop(); return;
    case hotkeySpace: changeShortcut(PreviewHotkey::space); return;
    case hotkeyCtrl: changeShortcut(PreviewHotkey::ctrl_space); return;
    case hotkeyCtrlShift: changeShortcut(PreviewHotkey::ctrl_shift_space); return;
    case hotkeyOff: changeShortcut(PreviewHotkey::disabled); return;
    case quit: PostQuitMessage(0); return;
    case speedSlow:
    case speedNormal:
    case speedFast:
    case speedDouble:
        if (player_) {
            constexpr std::array speeds{0.5, 1.0, 1.5, 2.0};
            player_->changeSpeed(speeds[id - speedSlow] / std::max(0.05, playback_.speed));
        }
        return;
    case seekBack:
    case seekForward: if (player_) player_->seekRelative(id == seekBack ? -5.0 : 5.0); return;
    default: return;
    }
    if (panel_ != Panel::hidden) lastPanel_ = panel_;
    sortCatalog(files_, sort_);
    savePreferences();
    layout();
    revealSelection();
}

std::optional<double> App::readRange(HWND field) const
{
    wchar_t buffer[32]{};
    if (GetWindowTextW(field, buffer, static_cast<int>(std::size(buffer))) == 0) return std::nullopt;
    std::wstring text(buffer);
    std::replace(text.begin(), text.end(), L',', L'.');
    const auto number = [](std::wstring_view part, bool fraction) -> double {
        if (part.empty() || part.find_first_not_of(fraction ? L"0123456789." : L"0123456789") != std::wstring_view::npos) {
            return -1;
        }
        const std::wstring input(part);
        wchar_t* end = nullptr;
        const double value = std::wcstod(input.c_str(), &end);
        return end == input.c_str() + input.size() && std::isfinite(value) ? value : -1;
    };
    const auto first = text.find(L':');
    if (first == std::wstring::npos) return number(text, true);
    const auto second = text.find(L':', first + 1);
    if (second == std::wstring::npos) return -1;
    std::wstring_view stamp(text);
    const double hours = number(stamp.substr(0, first), false);
    const double minutes = number(stamp.substr(first + 1, second - first - 1), false);
    const double seconds = number(stamp.substr(second + 1), true);
    if (hours < 0 || minutes < 0 || minutes >= 60 || seconds < 0 || seconds >= 60) return -1;
    return hours * 3600 + minutes * 60 + seconds;
}

void App::setRangeValue(HWND field, double seconds)
{
    const auto value = seconds >= 0 ? timeLabel(seconds, true) : std::wstring{};
    settingRange_ = true;
    SetWindowTextW(field, value.c_str());
    settingRange_ = false;
}

void App::useTimelinePosition()
{
    if (detail_ != Detail::exportGif || !player_ || !playback_.ready || exportThread_.joinable()) return;
    const auto state = player_->poll();
    const double position = state.duration > 0 ? std::clamp(state.position, 0.0, state.duration) : state.position;
    if (!std::isfinite(position) || position < 0) return;
    const HWND target = rangeTarget_ == rangeEnd_ ? rangeEnd_ : rangeStart_;
    if (target == rangeEnd_) rangeEndAutomatic_ = false;
    setRangeValue(target, position);
}

void App::startExport(GifPreset preset)
{
    if (kind_ != MediaKind::video || !playback_.ready || exportThread_.joinable()) return;
    std::optional<GifRange> range;
    const auto start = readRange(rangeStart_);
    const auto end = readRange(rangeEnd_);
    const double first = start.value_or(0.0);
    const double last = end.value_or(playback_.duration);
    const bool fullVideo = first == 0.0 && (!end ||
        (playback_.duration > 0 && std::abs(last - playback_.duration) <= 0.0005 + 1e-9));
    if (!fullVideo) {
        if (first < 0 || last - first < 0.01 || last > 864000.0 ||
            (playback_.duration > 0 && last > playback_.duration + 0.01)) {
            status_ = L"Укажите начало и конец в пределах длительности видео.";
            InvalidateRect(window_, &detailsRect_, FALSE);
            return;
        }
        range = GifRange{std::chrono::milliseconds(std::llround(first * 1000)),
                         std::chrono::milliseconds(std::llround((last - first) * 1000))};
    }

    const fs::path input = path_;
    auto suggested = input;
    suggested.replace_extension(L".gif");
    const auto output = dialogPath(window_, false, &suggested);
    if (!output) return;
    status_ = L"Экспорт GIF…";
    const HWND owner = window_;
    exportThread_ = std::jthread([this, owner, input, output = *output, preset, range](std::stop_token stop) {
        exportResult_ = convertToGif(input, output, preset, stop, range);
        PostMessageW(owner, exportMessage, 0, 0);
    });
    layout();
}

void App::acceptExport()
{
    if (!exportThread_.joinable()) return;
    exportThread_.join();
    status_ = std::move(exportResult_.message);
    layout();
    if (IsWindowVisible(window_)) {
        MessageBoxW(window_, status_.c_str(), L"Экспорт GIF", MB_OK |
                    (exportResult_.success ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    if (exportResult_.success && !directory_.empty()) {
        catalog_->scan(directory_, ++generation_);
        requested_.clear();
        thumbnails_.clear();
    }
}

struct Preferences {
    DWORD version = 1;
    int panel = 0;
    int sort = 0;
    int cards = 1;
    int thumb = 124;
    int width = 250;
    int height = 156;
    int onTop = 0;
    RECT quick{};
};

void App::loadPreferences()
{
    Preferences settings;
    DWORD size = sizeof(settings);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Viewery", L"Layout", RRF_RT_REG_BINARY,
                     nullptr, &settings, &size) != ERROR_SUCCESS || size != sizeof(settings) ||
        settings.version != 1) return;
    panel_ = static_cast<Panel>(std::clamp(settings.panel, 0, static_cast<int>(Panel::hidden)));
    if (panel_ != Panel::hidden) lastPanel_ = panel_;
    sort_ = static_cast<CatalogSort>(std::clamp(settings.sort, 0, static_cast<int>(CatalogSort::type)));
    cards_ = settings.cards != 0;
    thumb_ = std::clamp(settings.thumb, 86, 178);
    catalogWidth_ = std::clamp(settings.width, 172, 440);
    catalogHeight_ = std::clamp(settings.height, 95, 260);
    onTop_ = settings.onTop != 0;
    quickWindow_ = settings.quick;
    hasQuickSize_ = spanX(quickWindow_) > 0 && spanY(quickWindow_) > 0;
}

void App::savePreferences()
{
    const Preferences settings{1, static_cast<int>(panel_), static_cast<int>(sort_), cards_, thumb_,
                               catalogWidth_, catalogHeight_, onTop_, quickWindow_};
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Viewery", L"Layout", REG_BINARY, &settings, sizeof(settings));
}

bool App::receiveOpen(const COPYDATASTRUCT& data)
{
    if ((data.dwData != openMessage && data.dwData != quickMessage) || !data.lpData ||
        data.cbData < sizeof(wchar_t) ||
        data.cbData % sizeof(wchar_t) || data.cbData > 32768 * sizeof(wchar_t)) return false;
    const auto* value = static_cast<const wchar_t*>(data.lpData);
    const auto length = data.cbData / sizeof(wchar_t);
    if (value[length - 1] != L'\0') return false;
    const fs::path requested(value);
    if (!requested.empty() && !requested.is_absolute()) return false;
    if (data.dwData == quickMessage) explorer_->set_follow_selection(false);
    show(requested, data.dwData == quickMessage ? Mode::quick : Mode::full);
    return true;
}

LRESULT App::dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (window == controller_) {
        if (explorer_ && explorer_->handle_message(message, wparam, lparam)) return 0;
        if (message == WM_COPYDATA && lparam) return receiveOpen(*reinterpret_cast<const COPYDATASTRUCT*>(lparam));
        if (message == taskbarCreated_ && taskbarCreated_) { publishTray(true); return 0; }
        if (message == trayMessage) {
            if (LOWORD(lparam) == WM_CONTEXTMENU) {
                POINT point{};
                GetCursorPos(&point);
                openMenu(point, true);
            } else if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT) {
                show({}, Mode::full);
            }
            return 0;
        }
    }
    if (window != window_) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
    case WM_GETMINMAXINFO: {
        auto& sizing = *reinterpret_cast<MINMAXINFO*>(lparam);
        sizing.ptMinTrackSize = mode_ == Mode::full ? POINT{dp(670), dp(475)} : POINT{dp(365), dp(200)};
        return 0;
    }
    case WM_DPICHANGED: {
        if (switchingMode_) return 0;
        dpi_ = HIWORD(wparam);
        const auto& suggested = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested.left, suggested.top, spanX(suggested), spanY(suggested),
                     SWP_NOACTIVATE | SWP_NOZORDER);
        updateFonts();
        layout();
        return 0;
    }
    case WM_SIZE: if (!switchingMode_ && wparam != SIZE_MINIMIZED) layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_DRAWITEM:
        if (wparam >= static_cast<UINT>(Control::open) &&
            wparam <= static_cast<UINT>(Control::timestamp)) {
            paintButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (lparam && reinterpret_cast<const NMHDR*>(lparam)->hwndFrom == volumeTrack_ &&
            reinterpret_cast<const NMHDR*>(lparam)->code == NM_CUSTOMDRAW) {
            return paintVolume(*reinterpret_cast<const NMCUSTOMDRAW*>(lparam));
        }
        break;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == volumeTrack_) {
            setVolume(static_cast<double>(SendMessageW(volumeTrack_, TBM_GETPOS, 0, 0)));
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == rangeStart_ || reinterpret_cast<HWND>(lparam) == rangeEnd_) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, textColor);
            SetBkColor(dc, raised);
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == rangeStartId || LOWORD(wparam) == rangeEndId) {
            if (HIWORD(wparam) == EN_SETFOCUS) {
                rangeTarget_ = LOWORD(wparam) == rangeEndId ? rangeEnd_ : rangeStart_;
            } else if (HIWORD(wparam) == EN_CHANGE && LOWORD(wparam) == rangeEndId && !settingRange_) {
                rangeEndAutomatic_ = false;
            }
            if (!IsRectEmpty(&detailsRect_)) InvalidateRect(window_, &detailsRect_, FALSE);
            return 0;
        }
        if (LOWORD(wparam) >= static_cast<UINT>(Control::open) &&
            LOWORD(wparam) <= static_cast<UINT>(Control::timestamp) && HIWORD(wparam) == BN_CLICKED) {
            command(LOWORD(wparam));
            return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == refreshTimer) { refresh(); return 0; }
        if (wparam == canvasClickTimer && pendingClickTime_ &&
            GetTickCount64() - pendingClickTime_ >= GetDoubleClickTime()) {
            cancelCanvasClick();
            if (player_ && kind_ != MediaKind::image) player_->togglePause();
            return 0;
        }
        if (wparam == filenameTimer) {
            RECT bounds = filenameRect_;
            if (!IsRectEmpty(&bounds)) {
                if (IsRectEmpty(&playbackRect_)) InvalidateRect(window_, &bounds, FALSE);
                else {
                    OffsetRect(&bounds, -playbackRect_.left, -playbackRect_.top);
                    InvalidateRect(controlsStrip_, &bounds, FALSE);
                }
            }
            return 0;
        }
        break;
    case catalogMessage: if (acceptCatalog()) layout(); return 0;
    case exportMessage: acceptExport(); return 0;
    case promoteMessage:
        if (std::exchange(promotionPending_, false) && mode_ == Mode::quick && IsWindowVisible(window)) {
            show({}, Mode::full);
        }
        return 0;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        std::wstring file(DragQueryFileW(drop, 0, nullptr, 0) + 1, L'\0');
        const UINT length = DragQueryFileW(drop, 0, file.data(), static_cast<UINT>(file.size()));
        DragFinish(drop);
        file.resize(length);
        if (!file.empty()) show(fs::path(file), Mode::full);
        return 0;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (wparam == VK_SPACE && (lparam & (1LL << 30))) return 0;
        if (key(wparam, message == WM_SYSKEYDOWN && (lparam & (1LL << 29)))) return 0;
        break;
    case WM_MOUSEWHEEL: wheel(wparam, lparam); return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetFocus(window);
        click(point, static_cast<UINT>(wparam));
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (resizingPanel_) {
            if (panel_ == Panel::left) catalogWidth_ = std::clamp(MulDiv(point.x, 96, static_cast<int>(dpi_)), 172, 440);
            else catalogHeight_ = std::clamp(MulDiv(catalogRect_.bottom - point.y, 96, static_cast<int>(dpi_)), 95, 260);
            layout();
        } else if (scrubbing_ && player_) {
            const double ratio = static_cast<double>(point.x - timeline_.left) / std::max(1, spanX(timeline_));
            player_->seekAbsolute(playback_.duration * std::clamp(ratio, 0.0, 1.0));
        } else if (gesture_ != CanvasGesture::none) {
            moveCanvasGesture(point);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (gesture_ != CanvasGesture::none) {
            endCanvasGesture({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        }
        if (resizingPanel_) savePreferences();
        if (resizingPanel_ || scrubbing_) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        gesture_ = CanvasGesture::none;
        doubleClick_ = resizingPanel_ = scrubbing_ = false;
        return 0;
    case WM_CONTEXTMENU: {
        const HWND camera = buttons_[static_cast<UINT>(Control::capture) - static_cast<UINT>(Control::open)];
        if (reinterpret_cast<HWND>(wparam) == camera) {
            captureFrame(FrameCapturePurpose::save);
            return 0;
        }
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) GetCursorPos(&point);
        POINT local = point;
        ScreenToClient(window, &local);
        if (PtInRect(&catalogRect_, local)) catalogMenu(point);
        else openMenu(point, false);
        return 0;
    }
    case WM_CLOSE: hideViewer(); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int App::run()
{
    MSG event{};
    while (GetMessageW(&event, nullptr, 0, 0) > 0) {
        if (window_ && IsChild(window_, event.hwnd) && event.hwnd != rangeStart_ && event.hwnd != rangeEnd_ &&
            !(event.hwnd == volumeTrack_ && event.wParam >= VK_PRIOR && event.wParam <= VK_DOWN) &&
            (event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN) && event.wParam != VK_TAB) {
            if (event.wParam == VK_SPACE && (event.lParam & (1LL << 30))) continue;
            if (key(event.wParam, event.message == WM_SYSKEYDOWN && (event.lParam & (1LL << 29)))) continue;
        }
        if (window_ && IsWindowVisible(window_) && (event.message == WM_KEYDOWN && event.wParam == VK_TAB) &&
            (event.hwnd == window_ || IsChild(window_, event.hwnd))) {
            if (HWND next = GetNextDlgTabItem(window_, GetFocus(), (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
                SetFocus(next);
                continue;
            }
        }
        TranslateMessage(&event);
        DispatchMessageW(&event);
    }
    return static_cast<int>(event.wParam);
}

struct Launch {
    bool background = false;
    fs::path path;
    Mode mode = Mode::full;
};

Launch parseLaunch()
{
    int count = 0;
    LPWSTR* const args = CommandLineToArgvW(GetCommandLineW(), &count);
    Launch result;
    for (int i = 1; args && i < count; ++i) {
        const std::wstring_view arg(args[i]);
        if (arg == L"--background") result.background = true;
        else if ((arg == L"--open" || arg == L"--quick") && i + 1 < count) {
            result.path = args[++i];
            result.mode = arg == L"--quick" ? Mode::quick : Mode::full;
        }
        else if (!arg.starts_with(L"--")) result.path = args[i];
    }
    LocalFree(args);
    if (!result.path.empty()) {
        std::error_code error;
        const fs::path absolute = fs::absolute(result.path, error);
        if (!error) result.path = absolute.lexically_normal();
    }
    return result;
}

bool forwardLaunch(const Launch& launch)
{
    if (launch.background) return true;
    for (int retry = 0; retry < 20; ++retry) {
        if (HWND controller = FindWindowW(controllerClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(controller, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            auto message = launch.path.wstring();
            const auto kind = launch.mode == Mode::quick ? quickMessage : openMessage;
            COPYDATASTRUCT data{kind, static_cast<DWORD>((message.size() + 1) * sizeof(wchar_t)), message.data()};
            DWORD_PTR received = 0;
            return SendMessageTimeoutW(controller, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 1750, &received) && received;
        }
        Sleep(45);
    }
    return false;
}

}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    using namespace viewery;
    const auto launch = parseLaunch();
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Viewery.Instance.1");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const bool delivered = forwardLaunch(launch);
        CloseHandle(mutex);
        if (!delivered) MessageBoxW(nullptr, L"The running instance of Viewery is unavailable.", L"Viewery", MB_OK);
        return delivered ? 0 : 1;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) { CloseHandle(mutex); return 1; }
    int exitCode = 1;
    {
        App app(instance);
        if (App::registerClasses(instance) && app.start()) {
            if (!launch.background) app.show(launch.path, launch.mode);
            exitCode = app.run();
        }
    }
    CoUninitialize();
    CloseHandle(mutex);
    return exitCode;
}
