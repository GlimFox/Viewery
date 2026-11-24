#include "catalog.hpp"
#include "converter.hpp"
#include "explorer.hpp"
#include "frame_image.hpp"
#include "media.hpp"
#include "rename_studio.hpp"
#include "collection_view.hpp"
#include "updater.hpp"
#include "settings_window.hpp"
#include "quick_space.hpp"
#include "shell_selection.hpp"
#include "artist_registry.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
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
constexpr UINT libraryMessage = WM_APP + 24;
constexpr UINT updateMessage = WM_APP + 25;
constexpr UINT startupUpdateMessage = WM_APP + 26;
constexpr UINT artistMessage=WM_APP+27;
constexpr ULONG_PTR openMessage = 0x56574552;
constexpr ULONG_PTR quickMessage = 0x56574551;
constexpr UINT refreshTimer = 1;
constexpr UINT canvasClickTimer = 2;
constexpr UINT filenameTimer = 3;
#ifndef VIEWERY_VERSION
#define VIEWERY_VERSION "0.2.0-beta.7"
#endif

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
enum class Detail { none, information, exportGif, renameStudio };
enum class CanvasGesture { none, pressed, moveWindow, pan };
enum class Control : UINT {
    open = 100, catalog, info, exportGif, more, previous, back, play, step, next, speed,
    zoom, fullScreen, pin, promote, close, compact, balanced, quality, cancel, copy,
    panelPosition, volume, preset, advanced, startExport, capture, counter, timestamp, renameStudio,
    settings, catalogSelect, catalogArtists, catalogReveal
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
    L"Переключить время и кадры", L"Вставить позицию таймлайна в выбранное поле",
    L"Переименовать",L"Настройки",L"Выбор чекбоксами",L"Все файлы",L"Показать выбранные файлы в Проводнике"
};

enum Menu : UINT {
    openFile = 300, openFolder, panelLeft, panelBottom, panelHidden, listView, cardsView,
    smallThumb, mediumThumb, largeThumb, nameSort, dateSort, sizeSort, typeSort,
    hotkeySpace, hotkeyCtrl, hotkeyCtrlShift, hotkeyOff, quit, resetZoom,
    speedSlow, speedNormal, speedFast, speedDouble, seekBack, seekForward,
    selectionMode, selectionClear, selectionAll, selectionGroup,
    moduleRename, updatesCheck, updatesAutomatic, updatesInstall, updatesNotes, mediaToolsInstall,
    groupNone, groupArtist, groupWork, groupFormat, groupDirectory,
    sortArtist, sortNumber, sortTitle, sortVersion, sortCustom, filterUnassigned, filterUntitled, filterUnorganized,
    libraryRefresh, groupKeysForward, groupKeysReverse, libraryOpenStudio, openSettings, openTemplates,
    artistsAll, artistsGrouped, manageArtists
};

constexpr UINT rangeStartId = 600;
constexpr UINT rangeEndId = 601;
constexpr UINT volumeTrackId = 602;
constexpr UINT catalogSearchId = 603;
constexpr UINT catalogScrollId=604;

enum FileMenu : UINT {
    filePreview=1100,fileReveal,fileCopyPath,fileCopyName,fileCopy,fileProperties,fileSelect,fileRename
};

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
    case Control::renameStudio: return L"\xE8AC";
    case Control::settings:return L"\xE713";
    case Control::catalogSelect:return L"\xE739";
    case Control::catalogReveal:return L"\xE8DA";
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

bool arrangeChild(HWND child,RECT bounds,bool visible)
{
    if(!child) return false;
    RECT old{};GetWindowRect(child,&old);
    const HWND parent=GetParent(child);
    if(parent) MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&old),2);
    const bool wasVisible=(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0;
    const bool moved=!EqualRect(&old,&bounds);
    if(!moved&&wasVisible==visible) return false;
    SetWindowPos(child,nullptr,bounds.left,bounds.top,std::max(0,spanX(bounds)),std::max(0,spanY(bounds)),
        SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|(visible?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    if(parent) {InvalidateRect(parent,&old,FALSE);InvalidateRect(parent,&bounds,FALSE);}
    if(visible) InvalidateRect(child,nullptr,FALSE);
    return true;
}

void childVisible(HWND child,bool visible)
{
    if(child&&((GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)!=0)!=visible) ShowWindow(child,visible?SW_SHOWNA:SW_HIDE);
}

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

bool copyFile(HWND window,const fs::path& file)
{
    const auto value=file.wstring();
    const auto bytes=sizeof(DROPFILES)+(value.size()+2)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);
    if(!memory) return false;
    bool copied=false;
    if(auto* drop=static_cast<DROPFILES*>(GlobalLock(memory))) {
        drop->pFiles=sizeof(DROPFILES);drop->fWide=TRUE;
        std::copy_n(value.c_str(),value.size()+1,reinterpret_cast<wchar_t*>(drop+1));
        GlobalUnlock(memory);
        if(OpenClipboard(window)) {
            EmptyClipboard();
            if(SetClipboardData(CF_HDROP,memory)) {memory=nullptr;copied=true;}
            CloseClipboard();
        }
    }
    if(memory) GlobalFree(memory);
    return copied;
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
    explicit App(HINSTANCE instance) : instance_(instance) {collectionOptions_.filenameArtists=true;}
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
    static LRESULT CALLBACK catalogScrollProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data);

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
    RECT catalogViewport() const;
    bool createCatalogScroll(bool horizontal);
    void updateCatalogScroll();
    void paintCatalogScroll(HWND window);
    std::vector<fs::path> revealTargets() const;
    void revealCatalogSelection();
    void openArtistRegistry();
    void acceptArtistChanges();
    bool applyRenamedPaths(const std::vector<std::pair<fs::path,fs::path>>& changes);

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
    bool controlActive(Control control) const;
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
    void openStudio();
    void syncStudio();
    void beforeLibraryChanges();
    void acceptLibraryChanges();
    void rebuildCollectionView();
    void layoutCollectionTiles();
    bool groupedCatalog() const;
    int catalogHeaderHeight() const {return dp(renameEnabled_?(studio_?94:64):31);}
    void selectCatalog(int index,UINT keys,bool checkbox);
    int catalogItemAt(POINT point) const;
    RECT checkboxRect(RECT item) const;
    void closeStudio();
    void ensureStudio();
    void quickSpace(std::uint64_t timestamp);
    void openSettingsWindow();
    SettingsState settingsState() const;
    void settingsEvent(const SettingsEvent& event);
    void artistMenu(POINT point);
    void navigateKey(WPARAM code);
    void acceptUpdate();
    void navigateGroup(int direction);
    void updateCounter();
    void captureFrame(FrameCapturePurpose purpose);
    void acceptFrame(FrameCaptureResult result);
    bool key(WPARAM code, bool alt);
    void wheel(WPARAM wparam, LPARAM lparam);
    void command(UINT id);
    void openMenu(POINT point, bool tray);
    void catalogMenu(POINT point);
    HMENU makeFileMenu(const fs::path& file) const;
    void fileMenu(POINT point,const fs::path& file);
    void fileCommand(UINT command,const fs::path& file);
    void speedMenu(POINT point);
    void presetMenu(POINT point);
    void changeShortcut(PreviewHotkey shortcut);
    void publishTray(bool add);
    void loadPreferences();
    void savePreferences();
    bool receiveOpen(const COPYDATASTRUCT& data);

    std::wstring fileInfo() const;
    std::wstring statusSummary() const;
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
    std::array<bool,buttonLabels.size()> activeButtons_{};
    std::unique_ptr<ExplorerIntegration> explorer_;
    std::unique_ptr<CatalogWorker> catalog_;
    std::unique_ptr<MediaPlayer> player_;
    std::unique_ptr<RenameStudio> studio_;
    std::unique_ptr<UpdateService> updates_;
    std::unique_ptr<SettingsWindow> settings_;
    std::unique_ptr<ArtistRegistry> artists_;
    QuickSpaceGesture quickSpaceGesture_;
    UpdateInfo updateInfo_{};
    bool renameEnabled_=true,selectMode_=false;
    bool groupChordControl_=false;
    bool libraryChangesPending_=false,disableStudioPending_=false;
    std::optional<PlaybackState> resumePlayback_;
    int selectionAnchor_=-1;
    std::set<fs::path> selectedFiles_;
    CollectionViewOptions collectionOptions_;
    CollectionViewResult collectionView_;
    std::vector<RECT> collectionFileRects_;
    std::vector<std::pair<RECT,CollectionEntry>> collectionHeaders_;
    int collectionExtent_=0;
    bool applyingCollection_=false;
    HWND catalogSearch_=nullptr;
    HWND catalogScroll_=nullptr;
    bool catalogScrollDragging_=false;
    int catalogScrollOrigin_=0,catalogScrollStart_=0,catalogScrollHold_=0;
    std::vector<CatalogFile> directoryFiles_;
    std::map<UINT,std::pair<int,std::wstring>> catalogFilterCommands_;
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
    int detailsWidth_ = 510;
    bool selectFirst_ = false;
    bool cards_ = true;
    CatalogSort sort_ = CatalogSort::name;

    Mode mode_ = Mode::full;
    Panel panel_ = Panel::left;
    Panel lastPanel_ = Panel::left;
    Detail detail_ = Detail::none;
    RECT client_{}, mediaRect_{}, timeline_{}, catalogRect_{}, detailsRect_{}, divider_{}, detailsDivider_{},
         playbackRect_{}, statusRect_{}, headerRect_{}, filenameRect_{};
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    RECT quickWindow_{};
    POINT gestureOrigin_{}, gestureLast_{}, pendingClickPoint_{};
    RECT gestureWindow_{};
    ULONGLONG pendingClickTime_ = 0;
    ULONGLONG filenameSince_ = 0;
    CanvasGesture gesture_ = CanvasGesture::none;
    bool gestureCtrl_ = false;
    bool resizingDetails_ = false;
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
    artists_.reset();
    settings_.reset();
    studio_.reset();
    updates_.reset();
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

LRESULT CALLBACK App::catalogScrollProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data)
{
    auto& app=*reinterpret_cast<App*>(data);
    const bool horizontal=app.panel_==Panel::bottom;
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_PAINT) {app.paintCatalogScroll(window);return 0;}
    if(message==WM_MOUSEWHEEL) return SendMessageW(app.window_,message,wparam,lparam);
    if(message==WM_LBUTTONDOWN) {
        SCROLLBARINFO info{sizeof(info)};
        if(!GetScrollBarInfo(window,OBJID_CLIENT,&info)) return 0;
        const int position=horizontal?GET_X_LPARAM(lparam):GET_Y_LPARAM(lparam);
        RECT bounds{};GetClientRect(window,&bounds);
        const int length=horizontal?spanX(bounds):spanY(bounds);
        const int page=horizontal?spanX(app.catalogViewport()):spanY(app.catalogViewport());
        SetCapture(window);
        app.catalogScrollDragging_=position>=info.xyThumbTop&&position<info.xyThumbBottom;
        app.catalogScrollOrigin_=position;app.catalogScrollStart_=app.scroll_;
        if(!app.catalogScrollDragging_) {
            app.catalogScrollHold_=position<info.dxyLineButton?-app.dp(42):position>=length-info.dxyLineButton?app.dp(42):
                position<info.xyThumbTop?-page:page;
            app.scrollCatalog(app.catalogScrollHold_);SetTimer(window,1,300,nullptr);
        }
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    if(message==WM_MOUSEMOVE&&GetCapture()==window&&app.catalogScrollDragging_) {
        SCROLLBARINFO info{sizeof(info)};GetScrollBarInfo(window,OBJID_CLIENT,&info);
        RECT bounds{};GetClientRect(window,&bounds);
        const int length=horizontal?spanX(bounds):spanY(bounds);
        const int travel=std::max(1,length-info.dxyLineButton*2-(info.xyThumbBottom-info.xyThumbTop));
        const int position=horizontal?GET_X_LPARAM(lparam):GET_Y_LPARAM(lparam);
        const auto offset=static_cast<long long>(position-app.catalogScrollOrigin_)*app.maxScroll()/travel;
        const int target=static_cast<int>(std::clamp(static_cast<long long>(app.catalogScrollStart_)+offset,0LL,static_cast<long long>(app.maxScroll())));
        app.scrollCatalog(target-app.scroll_);return 0;
    }
    if(message==WM_TIMER&&wparam==1&&GetCapture()==window) {
        app.scrollCatalog(app.catalogScrollHold_);SetTimer(window,1,70,nullptr);return 0;
    }
    if(message==WM_LBUTTONUP||message==WM_CAPTURECHANGED) {
        KillTimer(window,1);app.catalogScrollDragging_=false;app.catalogScrollHold_=0;
        if(message==WM_LBUTTONUP&&GetCapture()==window) {ReleaseCapture();SetFocus(app.window_);}
        InvalidateRect(window,nullptr,FALSE);return 0;
    }
    return DefSubclassProc(window,message,wparam,lparam);
}

void App::paintCatalogScroll(HWND window)
{
    PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);
    RECT bounds{};GetClientRect(window,&bounds);fill(dc,bounds,surface);
    SCROLLBARINFO info{sizeof(info)};
    if(GetScrollBarInfo(window,OBJID_CLIENT,&info)) {
        const bool horizontal=panel_==Panel::bottom;
        RECT thumb=horizontal?RECT{info.xyThumbTop,dp(3),info.xyThumbBottom,bounds.bottom-dp(3)}:
            RECT{dp(3),info.xyThumbTop,bounds.right-dp(3),info.xyThumbBottom};
        if(!IsRectEmpty(&thumb)) rounded(dc,thumb,GetCapture()==window?accent:hint,GetCapture()==window?accent:hint,dp(6));
        const int size=info.dxyLineButton;
        const int middle=(horizontal?spanY(bounds):spanX(bounds))/2;
        HPEN pen=CreatePen(PS_SOLID,std::max(1,dp(1)),hint);const auto old=SelectObject(dc,pen);
        for(int end=0;end<2;++end) {
            const int along=end?(horizontal?spanX(bounds):spanY(bounds))-size/2:size/2;
            const int direction=end?1:-1;
            if(horizontal) {MoveToEx(dc,along-direction*dp(2),middle-dp(3),nullptr);LineTo(dc,along+direction*dp(1),middle);LineTo(dc,along-direction*dp(2),middle+dp(3));}
            else {MoveToEx(dc,middle-dp(3),along-direction*dp(2),nullptr);LineTo(dc,middle,along+direction*dp(1));LineTo(dc,middle+dp(3),along-direction*dp(2));}
        }
        SelectObject(dc,old);DeleteObject(pen);
    }
    EndPaint(window,&paint);
}

bool App::createCatalogScroll(bool horizontal)
{
    const HWND next=CreateWindowExW(0,L"SCROLLBAR",L"Прокрутка миниатюр",WS_CHILD|(horizontal?SBS_HORZ:SBS_VERT),
        0,0,0,0,window_,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(catalogScrollId)),instance_,nullptr);
    if(!next) return false;
    SetWindowTheme(next,L"DarkMode_Explorer",nullptr);
    SetWindowSubclass(next,catalogScrollProc,1,reinterpret_cast<DWORD_PTR>(this));
    if(catalogScroll_) {
        KillTimer(catalogScroll_,1);
        if(GetCapture()==catalogScroll_) ReleaseCapture();
        DestroyWindow(catalogScroll_);
    }
    catalogScrollDragging_=false;catalogScrollHold_=0;
    catalogScroll_=next;
    return true;
}

void App::updateCatalogScroll()
{
    if(!catalogScroll_) return;
    const RECT view=catalogViewport();
    const bool horizontal=panel_==Panel::bottom;
    const bool visible=mode_==Mode::full&&!fullscreen_&&panel_!=Panel::hidden&&!IsRectEmpty(&view)&&maxScroll()>0;
    if(!visible) {childVisible(catalogScroll_,false);return;}
    const LONG_PTR style=GetWindowLongPtrW(catalogScroll_,GWL_STYLE);
    // SCROLLBAR caches its orientation at creation; changing SBS_VERT alone
    // leaves native thumb geometry on the old axis, even after FRAMECHANGED.
    const bool orientationChanged=((style&SBS_VERT)==0)!=horizontal;
    if(orientationChanged&&!createCatalogScroll(horizontal)) {childVisible(catalogScroll_,false);return;}
    const RECT bounds=horizontal?RECT{view.left,view.bottom,catalogRect_.right,catalogRect_.bottom}:
        RECT{view.right,view.top,catalogRect_.right,view.bottom};
    const bool moved=arrangeChild(catalogScroll_,bounds,true);
    const int page=std::max(1,horizontal?spanX(view):spanY(view));
    SCROLLINFO previous{sizeof(previous),SIF_RANGE|SIF_PAGE|SIF_POS};GetScrollInfo(catalogScroll_,SB_CTL,&previous);
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMin=0;info.nMax=maxScroll()+page-1;
    info.nPage=static_cast<UINT>(page);info.nPos=scroll_;
    if(moved||orientationChanged||previous.nMax!=info.nMax||previous.nPage!=info.nPage||previous.nPos!=info.nPos) {
        // Realize native thumb geometry before our WM_PAINT override reads it.
        SetScrollInfo(catalogScroll_,SB_CTL,&info,TRUE);InvalidateRect(catalogScroll_,nullptr,FALSE);
    }
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
        },[this](std::uint64_t timestamp){quickSpace(timestamp);});
    if (!explorer_->start()) status_ = L"Сочетание Quick View недоступно. Измените его в меню Viewery.";
    loadPreferences();
    DWORD enabled=1,bytes=sizeof(enabled);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Viewery",L"RenameStudioEnabled",RRF_RT_REG_DWORD,
                    nullptr,&enabled,&bytes)==ERROR_SUCCESS) renameEnabled_=enabled!=0;
    updates_=std::make_unique<UpdateService>(controller_,updateMessage);
    publishTray(true);
    if(UpdateService::automaticChecks()) PostMessageW(controller_,startupUpdateMessage,0,0);
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
    controlsStrip_ = CreateWindowExW(0, stripClass, nullptr, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
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
    catalogSearch_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,
                                  0,0,0,0,window_,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(catalogSearchId)),
                                  instance_,nullptr);
    SendMessageW(catalogSearch_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Поиск"));
    if(!createCatalogScroll(false)) return false;
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
    if(mode==Mode::quick&&!IsWindowVisible(window_)) quickSpaceGesture_.reset();
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
    quickSpaceGesture_.reset();
    if(explorer_) explorer_->set_preview_active(mode==Mode::quick);
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
    quickSpaceGesture_.reset();
    if (fullscreen_) changeFullscreen();
    if (mode_ == Mode::quick) saveQuickSize();
    KillTimer(window_, refreshTimer);
    KillTimer(window_, filenameTimer);
    promotionPending_ = false;
    cancelCanvasClick();
    if (GetCapture() == window_) ReleaseCapture();
    if(explorer_) explorer_->set_follow_selection(false);
    if(studio_) studio_->hide();
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
    if(!samePath(path_,file)) quickSpaceGesture_.reset();
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
    if(!samePath(path_,file)) resumePlayback_.reset();
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
    syncStudio();
    layout();
}

void App::changeDirectory(const fs::path& directory, bool selectFirst)
{
    directory_ = directory;
    files_.clear();
    directoryFiles_.clear();
    selectedFiles_.clear();
    selectionAnchor_=-1;
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
    int next = current_ < 0 ? (direction > 0 ? 0 : last) : std::clamp(current_ + direction, 0, last);
    while(next!=current_&&samePath(files_[static_cast<std::size_t>(next)].path,path_)) {
        const int following=next+direction;
        if(following<0||following>last) return;
        next=following;
    }
    if (next == current_) return;
    const auto file = files_[static_cast<std::size_t>(next)].path;
    openMedia(file);
}

bool App::acceptCatalog()
{
    auto update = catalog_->take();
    if (update.generation != generation_) return false;
    if (update.scanned) {
        directoryFiles_=std::move(update.files);
        files_=directoryFiles_;
        sortCatalog(files_, sort_);
        if(studio_) {
            syncStudio();
            rebuildCollectionView();
        }
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

RECT App::catalogViewport() const
{
    RECT view=catalogRect_;
    if(IsRectEmpty(&view)) return {};
    view.top=std::min(view.bottom,view.top+catalogHeaderHeight());
    if(panel_==Panel::bottom) view.bottom=std::max(view.top,view.bottom-dp(15));
    else view.right=std::max(view.left,view.right-dp(15));
    return view;
}

int App::columns() const
{
    if (panel_ == Panel::bottom || !cards_) return 1;
    return std::max(1,(spanX(catalogViewport())-dp(20))/dp(thumb_));
}

int App::strideX() const
{
    if (panel_ == Panel::bottom) return dp(thumb_);
    return std::max(1,(spanX(catalogViewport())-dp(20))/columns());
}

int App::strideY() const
{
    return cards_ ? dp(thumb_ * 3 / 4 + 23) : dp(29);
}

RECT App::itemRect(std::size_t index) const
{
    if(groupedCatalog()&&index<collectionFileRects_.size()) {
        auto rect=collectionFileRects_[index];
        if(panel_==Panel::bottom) OffsetRect(&rect,-scroll_,0);
        else OffsetRect(&rect,0,-scroll_);
        return rect;
    }
    const RECT viewport=catalogViewport();
    const int count = static_cast<int>(index);
    if (panel_ == Panel::bottom) {
        const int x=viewport.left+dp(10)+count*strideX()-scroll_;
        return {x,viewport.top,x+strideX()-dp(7),viewport.bottom-dp(9)};
    }
    const int x=viewport.left+dp(10)+(count%columns())*strideX();
    const int y=viewport.top+(count/columns())*strideY()-scroll_;
    return {x, y, x + strideX() - dp(6), y + strideY() - dp(6)};
}

int App::maxScroll() const
{
    const RECT viewport=catalogViewport();
    if(groupedCatalog()) return std::max(0,collectionExtent_-
        (panel_==Panel::bottom?spanX(viewport):spanY(viewport)));
    const int count = static_cast<int>(files_.size());
    if(panel_==Panel::bottom) return std::max(0,count*strideX()+dp(12)-spanX(viewport));
    return std::max(0,((count+columns()-1)/columns())*strideY()+dp(8)-spanY(viewport));
}

void App::scrollCatalog(int distance)
{
    const int next=std::clamp(scroll_+distance,0,maxScroll());
    if(next==scroll_) return;
    scroll_=next;updateCatalogScroll();
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
        const RECT viewport=catalogViewport();
        const RECT itemBounds = itemRect(static_cast<std::size_t>(current_));
        if (panel_ == Panel::bottom) {
            if(itemBounds.left<viewport.left) scroll_+=itemBounds.left-viewport.left-dp(8);
            if(itemBounds.right>viewport.right) scroll_+=itemBounds.right-viewport.right+dp(8);
        } else {
            if(itemBounds.top<viewport.top) scroll_+=itemBounds.top-viewport.top;
            if(itemBounds.bottom>viewport.bottom) scroll_+=itemBounds.bottom-viewport.bottom+dp(8);
        }
    }
    scroll_ = std::clamp(scroll_, 0, maxScroll());
    updateCatalogScroll();
    requestThumbnails();
}

void App::requestThumbnails()
{
    if (!catalog_ || IsRectEmpty(&catalogRect_) || (!cards_ && panel_ != Panel::bottom)) return;
    const bool bottom = panel_ == Panel::bottom;
    const int stride = bottom ? strideX() : strideY();
    const int across = columns();
    const int first = groupedCatalog()?0:std::max(0, (scroll_ / stride - 1) * across);
    const int limit = groupedCatalog()?static_cast<int>(files_.size()):
        first + ((bottom ? spanX(catalogRect_) : spanY(catalogRect_)) / stride + 4) * across;
    std::vector<fs::path> pending;
    std::set<fs::path> visible;
    for (int index = first; index < limit && index < static_cast<int>(files_.size()); ++index) {
        if(groupedCatalog()) {
            auto viewport=catalogViewport();
            InflateRect(&viewport,dp(200),dp(200));
            RECT intersection{};
            const auto rect=itemRect(static_cast<std::size_t>(index));
            if(!IntersectRect(&intersection,&viewport,&rect)) continue;
        }
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
    SendMessageW(catalogSearch_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    for(HWND field:{rangeStart_,rangeEnd_,catalogSearch_})
        SendMessageW(field,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(dp(5),dp(5)));
    if (oldFont) DeleteObject(oldFont);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldIcon) DeleteObject(oldIcon);
}

void App::place(Control control, int x, int y, int width, int height, bool enabled)
{
    const auto index = static_cast<std::size_t>(static_cast<UINT>(control) - static_cast<UINT>(Control::open));
    HWND button = buttons_[index];
    if((IsWindowEnabled(button)!=FALSE)!=enabled) EnableWindow(button,enabled);
    arrangeChild(button,{x,y,x+width,y+height},true);
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
    const bool showStudio=fullUi&&detail_==Detail::renameStudio;
    const bool quickUi = mode_ == Mode::quick;
    const bool moving = kind_ && kind_ != MediaKind::image;
    const bool playbackBar = moving || (quickUi && kind_);
    const bool narrowQuick = quickUi && spanX(client_) < dp(570);

    mediaRect_ = client_;
    catalogRect_ = detailsRect_ = divider_ = detailsDivider_ = timeline_ = {};
    playbackRect_ = statusRect_ = headerRect_ = filenameRect_ = {};
    if (fullUi) {
        headerRect_ = {0, 0, client_.right, dp(40)};
        button(Control::open, dp(8), dp(4), 33);
        button(Control::catalog, dp(46), dp(4), 33);
        button(Control::panelPosition, dp(84), dp(4), 33);
        if(renameEnabled_) button(Control::renameStudio,dp(127),dp(4),132);
        button(Control::info, client_.right - dp(154), dp(4), 33, kind_.has_value());
        button(Control::exportGif, client_.right - dp(116), dp(4), 33,
               kind_ == MediaKind::video || exportThread_.joinable());
        button(Control::settings,client_.right-dp(78),dp(4),33);
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
        const int minimum=showStudio?148:172;
        const int remaining=showStudio?std::min(detailsWidth_+114,spanX(client_)*96/static_cast<int>(dpi_)-148):375;
        catalogRect_.right = dp(std::clamp(catalogWidth_,minimum,
            std::max(minimum,spanX(client_)*96/static_cast<int>(dpi_)-remaining)));
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
        detailsRect_.left = std::max(mediaRect_.left + dp(110),
            detailsRect_.right - dp(detail_==Detail::renameStudio?detailsWidth_:std::min(detailsWidth_,420)));
        detailsDivider_={detailsRect_.left-dp(5),detailsRect_.top,detailsRect_.left,detailsRect_.bottom};
        mediaRect_.right = detailsDivider_.left;
        if (panel_ == Panel::bottom) {
            catalogRect_.right = detailsDivider_.left;
            divider_.right = detailsDivider_.left;
        }
        const int x = detailsRect_.left + dp(16);
        const int width = std::max(98, MulDiv(spanX(detailsRect_), 96, static_cast<int>(dpi_)) - 32);
        if(detail_==Detail::renameStudio) {
            if(studio_) studio_->show(detailsRect_,dpi_,font_);
        } else if (detail_ == Detail::information) {
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
    const bool searchVisible=fullUi&&studio_&&renameEnabled_&&panel_!=Panel::hidden;
    if(fullUi&&panel_!=Panel::hidden) {
        const auto putCatalog=[&](Control control,RECT bounds,bool enabled=true) {
            visible[static_cast<UINT>(control)-static_cast<UINT>(Control::open)]=TRUE;
            place(control,bounds.left,bounds.top,spanX(bounds),spanY(bounds),enabled);
        };
        putCatalog(Control::catalogReveal,{catalogRect_.right-dp(renameEnabled_?92:60),catalogRect_.top+dp(2),catalogRect_.right-dp(renameEnabled_?64:32),catalogRect_.top+dp(28)},!revealTargets().empty());
        if(renameEnabled_) {
        putCatalog(Control::catalogSelect,{catalogRect_.right-dp(61),catalogRect_.top+dp(2),catalogRect_.right-dp(33),catalogRect_.top+dp(28)});
        putCatalog(Control::catalogArtists,{catalogRect_.left+dp(8),catalogRect_.top+dp(33),catalogRect_.right-dp(8),catalogRect_.top+dp(59)});
        const auto caption=collectionOptions_.group==CollectionGroup::artist?L"Художники ▾":L"Все файлы ▾";
        const HWND artistsButton=buttons_[static_cast<UINT>(Control::catalogArtists)-static_cast<UINT>(Control::open)];
        wchar_t old[64]{};GetWindowTextW(artistsButton,old,64);
        if(wcscmp(old,caption)) SetWindowTextW(artistsButton,caption);
        }
    }
    if(searchVisible) {
        arrangeChild(catalogSearch_,{catalogRect_.left+dp(8),catalogRect_.top+dp(65),catalogRect_.right-dp(8),catalogRect_.top+dp(90)},true);
    } else childVisible(catalogSearch_,false);
    if(studio_&&(!showDetails||detail_!=Detail::renameStudio)) studio_->hide();
    const bool rangeVisible = showDetails && detail_ == Detail::exportGif;
    if (rangeVisible) {
        const int x = detailsRect_.left + dp(16);
        const int y = detailsRect_.top + dp(185);
        const int width = (spanX(detailsRect_) - dp(78)) / 2;
        arrangeChild(rangeStart_,{x,y,x+width,y+dp(28)},true);
        arrangeChild(rangeEnd_,{x+width+dp(10),y,x+width*2+dp(10),y+dp(28)},true);
        button(Control::timestamp, detailsRect_.right - dp(42), y - dp(2), 28,
               playback_.ready && !exportThread_.joinable());
    }
    childVisible(rangeStart_,rangeVisible);childVisible(rangeEnd_,rangeVisible);
    if((IsWindowEnabled(rangeStart_)!=FALSE)==exportThread_.joinable()) EnableWindow(rangeStart_,!exportThread_.joinable());
    if((IsWindowEnabled(rangeEnd_)!=FALSE)==exportThread_.joinable()) EnableWindow(rangeEnd_,!exportThread_.joinable());

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
            arrangeChild(volumeTrack_,{tools,y+dp(3),tools+dp(sliderWidth),y+dp(29)},true);
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
        int summaryWidth=dp(190);
        if(HDC dc=GetDC(window_)) {
            const auto oldFont=SelectObject(dc,font_);
            const auto summary=statusSummary();SIZE size{};
            GetTextExtentPoint32W(dc,summary.c_str(),static_cast<int>(summary.size()),&size);
            summaryWidth=std::clamp(static_cast<int>(size.cx)+dp(32),dp(190),dp(320));
            SelectObject(dc,oldFont);ReleaseDC(window_,dc);
        }
        filenameRect_ = {std::min(summaryWidth,static_cast<int>(statusRect_.right)-dp(280)),
            statusRect_.top + 1,statusRect_.right - dp(106),statusRect_.bottom};
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
    if(!showVolume) childVisible(volumeTrack_,false);

    const auto showStrip = [&](HWND strip, RECT rect) {
        if (!strip) return;
        if (IsRectEmpty(&rect)) {
            childVisible(strip,false);
        } else {
            arrangeChild(strip,rect,true);
        }
    };
    showStrip(controlsStrip_, playbackRect_);
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        if(!visible[index]) childVisible(buttons_[index],false);
        const bool active=controlActive(static_cast<Control>(static_cast<UINT>(Control::open)+static_cast<UINT>(index)));
        if(activeButtons_[index]!=active) {activeButtons_[index]=active;InvalidateRect(buttons_[index],nullptr,FALSE);}
    }

    const bool showMedia = player_ && kind_ && !playback_.failed && spanX(mediaRect_) > 0 && spanY(mediaRect_) > 0;
    arrangeChild(video_,mediaRect_,showMedia);
    layoutCollectionTiles();
    scroll_ = std::clamp(scroll_, 0, maxScroll());
    updateCatalogScroll();
    requestThumbnails();
    updateCounter();
    InvalidateRect(window_, nullptr, FALSE);
}

bool App::controlActive(Control id) const
{
    return (id==Control::catalog&&panel_!=Panel::hidden)||
        (id==Control::info&&detail_==Detail::information)||(id==Control::exportGif&&detail_==Detail::exportGif)||
        (id==Control::pin&&onTop_)||(id==Control::renameStudio&&detail_==Detail::renameStudio)||
        (id==Control::catalogSelect&&selectMode_)||(id==Control::catalogArtists&&collectionOptions_.group==CollectionGroup::artist)||
        (id==Control::settings&&settings_&&settings_->visible());
}

void App::paintButton(const DRAWITEMSTRUCT& item)
{
    const Control id = static_cast<Control>(item.CtlID);
    const bool disabled = item.itemState & ODS_DISABLED;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hover = item.itemState & ODS_HOTLIGHT;
    const bool active=controlActive(id);
    const bool primary = id == Control::play || id == Control::startExport;
    const bool card = id == Control::preset || id == Control::advanced || id == Control::copy ||
                      id == Control::cancel || id == Control::timestamp || primary;
    const auto icon = id==Control::catalogSelect&&selectMode_?std::wstring_view(L"\xE73A"):id==Control::renameStudio?std::wstring_view{}:id == Control::volume && playback_.volume <= 0
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
    const RECT title{catalogRect_.left + dp(9), catalogRect_.top,
                     catalogRect_.right - dp(renameEnabled_?94:62), catalogRect_.top + dp(30)};
    const auto count=std::to_wstring(studio_&&renameEnabled_?collectionView_.fileCount:files_.size());
    const auto selected=selectedFiles_.empty()?L"":L" · ✓"+std::to_wstring(selectedFiles_.size());
    const auto caption=spanX(title)>=dp(140)?L"Каталог · "+count+selected:
        spanX(title)>=dp(100)?L"Файлы · "+count:L"Файлы";
    label(dc,caption,title,textColor);
    label(dc,L"···",{catalogRect_.right-dp(29),catalogRect_.top,catalogRect_.right-dp(6),catalogRect_.top+dp(30)},hint,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    const int saved = SaveDC(dc);
    const RECT viewport=catalogViewport();
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    if(groupedCatalog()) for(const auto& [bounds,entry]:collectionHeaders_) {
        auto rect=bounds;
        if(panel_==Panel::bottom) OffsetRect(&rect,-scroll_,0);else OffsetRect(&rect,0,-scroll_);
        fill(dc,rect,raised);
        const auto arrow=collectionOptions_.collapsed.contains(entry.key)?L"› ":L"⌄ ";
        rect.left+=dp(7+static_cast<int>(entry.ancestorKeys.size())*8);
        label(dc,arrow+entry.label,rect,accentHover);
    }
    const bool bottom = panel_ == Panel::bottom;
    const int stride = bottom ? strideX() : strideY();
    const int first = groupedCatalog()?0:std::max(0, (scroll_ / stride - 1) * columns());
    const int limit = groupedCatalog()?static_cast<int>(files_.size()):std::min(static_cast<int>(files_.size()),
        first + ((bottom ? spanX(catalogRect_) : spanY(catalogRect_)) / stride + 4) * columns());
    const HDC memory = CreateCompatibleDC(dc);
    for (int index = first; index < limit; ++index) {
        const auto& file = files_[static_cast<std::size_t>(index)];
        const RECT rect = itemRect(static_cast<std::size_t>(index));
        RECT intersection{};
        if(!IntersectRect(&intersection,&rect,&viewport)) continue;
        const bool selectedFile=selectedFiles_.contains(file.path);
        rounded(dc, rect, selectedFile?RGB(38,51,65):index == current_ ? activeSurface : background,
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
            const int inset=renameEnabled_&&(selectMode_||selectedFile)?30:8;
            label(dc,file.name,{rect.left+dp(inset),rect.top,rect.right-dp(5),rect.bottom});
        }
        if(renameEnabled_&&(selectMode_||selectedFile)) {
            RECT check=checkboxRect(rect);
            rounded(dc,check,selectedFile?accent:surface,selectedFile?accent:hint,dp(4));
            if(selectedFile) label(dc,L"✓",check,background,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
    }
    DeleteDC(memory);
    RestoreDC(dc, saved);
}

std::wstring App::statusSummary() const
{
    if(!status_.empty()) return status_;
    if(!playback_.width||!playback_.height) return {};
    auto value=std::to_wstring(playback_.width)+L" × "+std::to_wstring(playback_.height);
    if(!playback_.videoCodec.empty()) value+=L"   ·   "+playback_.videoCodec;
    if(playback_.fps>0&&kind_!=MediaKind::image) value+=L"   ·   "+decimal(playback_.fps,1)+L" FPS";
    return value;
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
    if (IsRectEmpty(&detailsRect_)||detail_==Detail::renameStudio) return;
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
    if(!IsRectEmpty(&detailsDivider_)) fill(dc,detailsDivider_,resizingDetails_?accent:border);
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
        const auto summary=statusSummary();
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
        if(resumePlayback_&&playback_.ready) {
            const auto resume=std::exchange(resumePlayback_,std::nullopt);
            player_->changeVolume(resume->volume-playback_.volume);
            player_->changeZoom(std::log2(std::max(0.25,resume->zoom)/std::max(0.25,playback_.zoom)));
            if(kind_!=MediaKind::image) {
                player_->seekAbsolute(resume->position);
                if(playback_.paused!=resume->paused) player_->togglePause();
                player_->changeSpeed(resume->speed/std::max(0.05,playback_.speed));
            }
        }
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
        if(failChanged||readyChanged||(infoChanged&&kind_==MediaKind::image)) layout();
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
    if(foreground!=window_&&foreground!=source) quickSpaceGesture_.reset();
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
    } else if(PtInRect(&detailsDivider_,point)) {
        SetCapture(window_);resizingDetails_=true;
    } else if (PtInRect(&catalogRect_, point)) {
        if (point.y < catalogRect_.top + dp(30)) {
            ClientToScreen(window_, &point);
            catalogMenu(point);
            return;
        }
        if(point.y<catalogRect_.top+catalogHeaderHeight()) return;
        if(groupedCatalog()) {
            for(const auto& [bounds,entry]:collectionHeaders_) {
                auto rect=bounds;
                if(panel_==Panel::bottom) OffsetRect(&rect,-scroll_,0);else OffsetRect(&rect,0,-scroll_);
                if(PtInRect(&rect,point)) {
                    if(!collectionOptions_.collapsed.erase(entry.key)) collectionOptions_.collapsed.insert(entry.key);
                    rebuildCollectionView();layout();return;
                }
            }
        }
        if(const int index=catalogItemAt(point);index>=0) {
            const auto& file=files_[static_cast<std::size_t>(index)];
            RECT check=checkboxRect(itemRect(static_cast<std::size_t>(index)));
            InflateRect(&check,dp(4),dp(4));
            const bool checkbox=renameEnabled_&&(selectMode_||selectedFiles_.contains(file.path))&&PtInRect(&check,point);
            selectCatalog(index,buttons,checkbox);
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
        gesture_ = (mode_==Mode::full&&!gestureCtrl_)||(mode_==Mode::quick&&gestureCtrl_)?CanvasGesture::pan:CanvasGesture::moveWindow;
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
    if(code!=VK_SPACE) quickSpaceGesture_.reset();
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if(renameEnabled_&&mode_==Mode::full&&ctrl&&code=='A') {
        command(selectionAll);return true;
    }
    if(renameEnabled_&&alt&&(code==VK_UP||code==VK_DOWN)&&ctrl==groupChordControl_) {
        navigateGroup(code==VK_UP?-1:1);return true;
    }
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
    if(code==VK_SPACE&&!alt&&!ctrl) {
        if(mode_==Mode::quick) quickSpace(GetTickCount64());
        else if(player_&&kind_!=MediaKind::image) player_->togglePause();
        return true;
    }
    if (code == VK_LEFT || code == VK_RIGHT) {
        const int direction = code == VK_LEFT ? -1 : 1;
        if(shift&&player_&&kind_!=MediaKind::image) player_->seekRelative(direction*5.0);
        else if(alt&&player_&&kind_==MediaKind::image) player_->pan(direction*0.08,0.0);
        else if(alt&&player_) player_->stepFrame(direction);
        else if(!alt&&!ctrl) navigateKey(code);
        return true;
    }
    if((code==VK_UP||code==VK_DOWN)&&!alt&&!ctrl) {navigateKey(code);return true;}
    if (!player_ || alt || ctrl) return false;
    if (code == VK_OEM_COMMA || code == VK_OEM_PERIOD) {
        if (kind_ == MediaKind::image) return false;
        player_->stepFrame(code == VK_OEM_COMMA ? -1 : 1);
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
    if(PtInRect(&detailsRect_,point)) {
        if(studio_&&detail_==Detail::renameStudio) SendMessageW(studio_->window(),WM_MOUSEWHEEL,wparam,lparam);
        return;
    }
    if (PtInRect(&catalogRect_, point)) {
        if (keys & MK_CONTROL) {
            thumb_ = std::clamp(thumb_ + static_cast<int>(steps * 12), 86, 178);
            savePreferences();
            layout();
            revealSelection();
        } else scrollCatalog(-static_cast<int>(steps * dp(88)));
    } else if(player_&&PtInRect(&mediaRect_,point)) {
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

void App::quickSpace(std::uint64_t timestamp)
{
    if(mode_!=Mode::quick||!window_||!IsWindowVisible(window_)) {quickSpaceGesture_.reset();return;}
    const auto action=quickSpaceGesture_.press(timestamp,GetDoubleClickTime());
    if(action==QuickSpaceAction::closePreview) hideViewer();
    else if(action==QuickSpaceAction::togglePlayback&&player_&&kind_!=MediaKind::image) player_->togglePause();
}

void App::navigateKey(WPARAM code)
{
    const int direction=code==VK_LEFT||code==VK_UP?-1:1;
    if((code==VK_UP||code==VK_DOWN)&&panel_==Panel::left&&cards_&&!groupedCatalog()) {
        if(files_.empty()) return;
        const int target=std::clamp(current_+direction*columns(),0,static_cast<int>(files_.size())-1);
        if(target!=current_) {const auto file=files_[static_cast<std::size_t>(target)].path;openMedia(file);syncStudio();}
    } else {navigate(direction);syncStudio();}
}

void App::ensureStudio()
{
    if(!renameEnabled_) return;
    if(!studio_) studio_=std::make_unique<RenameStudio>(window_,libraryMessage,[this]{beforeLibraryChanges();});
    syncStudio();
}

void App::openStudio()
{
    if(!renameEnabled_||mode_!=Mode::full||fullscreen_) return;
    ensureStudio();
    detail_=Detail::renameStudio;
    layout();
}

void App::closeStudio()
{
    if(detail_!=Detail::renameStudio) return;
    detail_=Detail::none;
    if(studio_) studio_->hide();
    SetFocus(window_);
    layout();
}

SettingsState App::settingsState() const
{
    SettingsState state;
    const bool updating=updates_&&updates_->busy();
    const auto update=updating?updates_->take():updateInfo_;
    state.quickHotkey=explorer_?explorer_->hotkey():PreviewHotkey::space;
    state.automaticUpdates=UpdateService::automaticChecks();
    state.renameEnabled=renameEnabled_;state.groupControlChord=groupChordControl_;
    state.updateBusy=updating;
    state.canInstall=update.status==UpdateStatus::available&&update.canInstall;
    state.canShowReleaseNotes=!update.notes.empty();
    state.version=L"" VIEWERY_VERSION;state.updateStatus=update.message;
    return state;
}

void App::openSettingsWindow()
{
    if(!window_&&!makeViewer()) return;
    if(!settings_) settings_=std::make_unique<SettingsWindow>(window_,[this](const SettingsEvent& event){settingsEvent(event);});
    settings_->show(settingsState(),dpi_);
    InvalidateRect(buttons_[static_cast<UINT>(Control::settings)-static_cast<UINT>(Control::open)],nullptr,FALSE);
}

void App::settingsEvent(const SettingsEvent& event)
{
    switch(event.action) {
    case SettingsAction::quickHotkey:
        if(explorer_&&event.value>=0&&event.value<=static_cast<int>(PreviewHotkey::disabled)) changeShortcut(static_cast<PreviewHotkey>(event.value));
        break;
    case SettingsAction::groupChord:groupChordControl_=event.value!=0;savePreferences();break;
    case SettingsAction::automaticUpdates:UpdateService::setAutomaticChecks(event.value!=0);break;
    case SettingsAction::renameModule:if(renameEnabled_!=(event.value!=0)) command(moduleRename);break;
    case SettingsAction::checkUpdates:command(updatesCheck);break;
    case SettingsAction::installUpdate:command(updatesInstall);break;
    case SettingsAction::releaseNotes:command(updatesNotes);break;
    case SettingsAction::mediaTools:command(mediaToolsInstall);break;
    case SettingsAction::templates:command(openTemplates);break;
    case SettingsAction::artists:command(manageArtists);break;
    case SettingsAction::backupCollection:case SettingsAction::exportMetadata:case SettingsAction::importMetadata:
        if(!renameEnabled_) {MessageBoxW(settings_->window(),L"Включите модуль коллекции для работы с базой.",L"Коллекция",MB_OK);break;}
        ensureStudio();
        studio_->collectionAction(event.action==SettingsAction::backupCollection?RenameStudio::CollectionAction::backup:
            event.action==SettingsAction::exportMetadata?RenameStudio::CollectionAction::exportMetadata:RenameStudio::CollectionAction::importMetadata);
        break;
    case SettingsAction::repository:ShellExecuteW(settings_->window(),L"open",L"https://github.com/GlimFox/Viewery",nullptr,nullptr,SW_SHOWNORMAL);break;
    }
    if(settings_) settings_->update(settingsState());
}

void App::syncStudio()
{
    if(!studio_||!renameEnabled_) return;
    std::vector<fs::path> paths;
    for(const auto& file:directoryFiles_) paths.push_back(file.path);
    std::vector<fs::path> selected(selectedFiles_.begin(),selectedFiles_.end());
    if(selected.empty()&&!selectMode_&&!path_.empty()) selected.push_back(path_);
    studio_->setFiles(std::move(paths),std::move(selected));
}

void App::beforeLibraryChanges()
{
    if(exportThread_.joinable()) throw LibraryError("Завершите экспорт GIF перед переименованием файлов");
    if(artists_&&artists_->busy()) throw LibraryError("Завершите операцию с художником перед переименованием файлов");
    libraryChangesPending_=true;
    if(player_) {
        resumePlayback_=player_->poll();
        player_.reset();
    }
}

bool App::applyRenamedPaths(const std::vector<std::pair<fs::path,fs::path>>& changes)
{
    // Resolve every original path once: chains and cycles must not move A twice.
    const auto mapped=[&](const fs::path& original) {
        for(const auto& [before,after]:changes) if(samePath(original,before)) return after;
        return original;
    };
    const auto viewed=mapped(path_);
    const bool moved=viewed!=path_;
    path_=viewed;
    std::set<fs::path> selected;
    for(const auto& file:selectedFiles_) selected.insert(mapped(file));
    selectedFiles_=std::move(selected);
    for(auto* entries:{&directoryFiles_,&files_}) for(auto& entry:*entries) {
        entry.path=mapped(entry.path);entry.name=entry.path.filename().wstring();
        entry.extension=entry.path.extension().wstring();
    }
    decltype(thumbnails_) thumbnails;
    for(auto& [file,thumbnail]:thumbnails_) thumbnails.emplace(mapped(file),std::move(thumbnail));
    thumbnails_=std::move(thumbnails);requested_.clear();
    filenameSince_=GetTickCount64();updateTitle();
    return moved;
}

std::vector<fs::path> App::revealTargets() const
{
    if(!selectedFiles_.empty()) return {selectedFiles_.begin(),selectedFiles_.end()};
    return path_.empty()?std::vector<fs::path>{}:std::vector<fs::path>{path_};
}

void App::revealCatalogSelection()
{
    const auto files=revealTargets();
    if(!files.empty()&&FAILED(revealInExplorer(files)))
        MessageBoxW(window_,L"Не удалось выделить файлы в Проводнике. Возможно, файл был перемещён.",L"Расположение файлов",MB_OK|MB_ICONWARNING);
}

void App::openArtistRegistry()
{
    if(!renameEnabled_) return;
    if(!window_&&!makeViewer()) return;
    if(!artists_) artists_=std::make_unique<ArtistRegistry>(window_,artistMessage,[this] {
        if(studio_&&(studio_->busy()||studio_->queuedFileCount()))
            throw LibraryError("Завершите или очистите очередь переименования перед изменением художника");
        beforeLibraryChanges();
    });
    artists_->show(dpi_);
}

void App::acceptArtistChanges()
{
    if(!artists_) return;
    if(const auto changes=artists_->takeFileChanges()) {
        applyRenamedPaths(changes->changes);
        if(!changes->message.empty()) status_=changes->message;
    }
    if(std::exchange(libraryChangesPending_,false)) {
        const auto file=path_;
        if(!file.empty()&&!player_) openMedia(file);
        if(catalog_) catalog_->scan(directory_,++generation_);
    }
    collectionOptions_.filter.artist.clear();
    if(studio_) {syncStudio();studio_->refreshCollection();}
    rebuildCollectionView();layout();
}

void App::acceptLibraryChanges()
{
    if(!studio_) return;
    if(studio_->takeBatchQueued()) {
        selectedFiles_.clear();selectionAnchor_=-1;
        syncStudio();
    }
    const auto changes=studio_->takeFileChanges();
    bool viewedFileMoved=false;
    if(changes) {
        viewedFileMoved=applyRenamedPaths(changes->changes);
        if(!changes->message.empty()) status_=changes->message;
    }
    if(std::exchange(libraryChangesPending_,false)) {
        auto path=path_;
        if(!path.empty()&&(!player_||viewedFileMoved)) openMedia(path);
        catalog_->scan(directory_,++generation_);
    }
    if(artists_) artists_->refresh();
    rebuildCollectionView();
    if(disableStudioPending_&&!studio_->busy()) {
        disableStudioPending_=false;
        if(!studio_->queuedFileCount()) studio_.reset();
        files_=directoryFiles_;
        sortCatalog(files_,sort_);
        collectionView_={};collectionFileRects_.clear();collectionHeaders_.clear();
    }
    layout();
}

bool App::groupedCatalog() const
{
    return studio_&&renameEnabled_&&collectionOptions_.group!=CollectionGroup::none;
}

void App::rebuildCollectionView()
{
    if(!studio_||!renameEnabled_||applyingCollection_) return;
    applyingCollection_=true;
    std::vector<fs::path> paths;
    std::map<fs::path,CatalogFile> entries;
    for(const auto& file:directoryFiles_) {paths.push_back(file.path);entries.emplace(file.path,file);}
    collectionView_=buildCollectionView(studio_->snapshot(),paths,collectionOptions_);
    files_.clear();
    for(const auto& entry:collectionView_.rows) if(!entry.group) {
        const auto found=entries.find(entry.path);
        if(found!=entries.end()) files_.push_back(found->second);
    }
    requested_.clear();
    layoutCollectionTiles();
    revealSelection();
    applyingCollection_=false;
}

void App::layoutCollectionTiles()
{
    collectionFileRects_.clear();
    collectionHeaders_.clear();
    collectionExtent_=0;
    const RECT viewport=catalogViewport();
    if(!groupedCatalog()||IsRectEmpty(&viewport)) return;
    const bool bottom=panel_==Panel::bottom;
    int cursor=bottom?dp(10):0;
    for(const auto& entry:collectionView_.rows) {
        const int indent=bottom?0:dp(6+static_cast<int>(entry.ancestorKeys.size())*7);
        if(entry.group) {
            RECT rect;
            if(bottom) rect={viewport.left+cursor,viewport.top,
                viewport.left+cursor+dp(166),viewport.bottom-dp(7)};
            else rect={viewport.left+indent,viewport.top+cursor,viewport.right-dp(7),
                viewport.top+cursor+dp(27)};
            collectionHeaders_.emplace_back(rect,entry);
            cursor+=bottom?dp(173):dp(30);
        } else {
            RECT rect;
            if(bottom) rect={viewport.left+cursor,viewport.top,
                viewport.left+cursor+dp(thumb_-7),viewport.bottom-dp(7)};
            else rect={viewport.left+indent,viewport.top+cursor,viewport.right-dp(7),
                viewport.top+cursor+strideY()-dp(6)};
            collectionFileRects_.push_back(rect);
            cursor+=bottom?dp(thumb_):strideY();
        }
    }
    collectionExtent_=cursor+dp(8);
}

RECT App::checkboxRect(RECT item) const
{
    return {item.left+dp(5),item.top+dp(4),item.left+dp(22),item.top+dp(21)};
}

int App::catalogItemAt(POINT point) const
{
    const auto viewport=catalogViewport();
    if(!PtInRect(&viewport,point)) return -1;
    if(groupedCatalog()) {
        for(std::size_t index=0;index<files_.size();++index) {
            const auto bounds=itemRect(index);
            if(PtInRect(&bounds,point)) return static_cast<int>(index);
        }
        return -1;
    }
    const bool bottom=panel_==Panel::bottom;
    const int index=bottom?(point.x-catalogRect_.left-dp(10)+scroll_)/strideX():
        ((point.y-catalogRect_.top-catalogHeaderHeight()+scroll_)/strideY())*columns()+
        (point.x-catalogRect_.left-dp(10))/strideX();
    if(index<0||index>=static_cast<int>(files_.size())) return -1;
    const auto bounds=itemRect(static_cast<std::size_t>(index));
    return PtInRect(&bounds,point)?index:-1;
}

void App::selectCatalog(int index,UINT keys,bool checkbox)
{
    if(index<0||index>=static_cast<int>(files_.size())) return;
    const auto file=files_[static_cast<std::size_t>(index)].path;
    if(renameEnabled_&&(checkbox||(keys&(MK_CONTROL|MK_SHIFT)))) {
        if((keys&MK_SHIFT)&&selectionAnchor_>=0&&selectionAnchor_<static_cast<int>(files_.size())) {
            for(int item=std::min(index,selectionAnchor_);item<=std::max(index,selectionAnchor_);++item) {
                selectedFiles_.insert(files_[static_cast<std::size_t>(item)].path);
            }
        } else if(!selectedFiles_.erase(file)) selectedFiles_.insert(file);
        selectionAnchor_=index;
        syncStudio();
        InvalidateRect(window_,&catalogRect_,FALSE);
        return;
    }
    selectionAnchor_=index;
    openMedia(file);
    syncStudio();
}

void App::navigateGroup(int direction)
{
    if(!studio_||collectionOptions_.group!=CollectionGroup::artist) return;
    std::wstring currentGroup;
    std::vector<std::pair<std::wstring,fs::path>> groups;
    for(const auto& entry:collectionView_.rows) {
        if(entry.group&&entry.ancestorKeys.empty()) currentGroup=entry.key;
        else if(!entry.group&&!currentGroup.empty()&&(groups.empty()||groups.back().first!=currentGroup)) {
            groups.emplace_back(currentGroup,entry.path);
        }
    }
    if(groups.empty()) return;
    int current=0;
    currentGroup.clear();
    for(const auto& entry:collectionView_.rows) {
        if(entry.group&&entry.ancestorKeys.empty()) currentGroup=entry.key;
        if(!entry.group&&samePath(entry.path,path_)) {
            const auto found=std::ranges::find(groups,currentGroup,&std::pair<std::wstring,fs::path>::first);
            if(found!=groups.end()) current=static_cast<int>(found-groups.begin());
            break;
        }
    }
    const int next=std::clamp(current+direction,0,static_cast<int>(groups.size())-1);
    openMedia(groups[static_cast<std::size_t>(next)].second);
}

void App::acceptUpdate()
{
    updateInfo_=updates_->take();
    if(settings_) settings_->update(settingsState());
    if(updateInfo_.status==UpdateStatus::mediaReady&&window_&&!path_.empty()) {
        const auto file=path_;player_.reset();openMedia(file);
    }
    if(!updateInfo_.message.empty()&&(updateInfo_.manual||updateInfo_.status==UpdateStatus::available||
                                    updateInfo_.status==UpdateStatus::mediaReady)) status_=updateInfo_.message;
    if(window_) InvalidateRect(window_,&statusRect_,FALSE);
    if(updateInfo_.manual&&updateInfo_.status!=UpdateStatus::checking&&
       updateInfo_.status!=UpdateStatus::installing&&updateInfo_.status!=UpdateStatus::mediaInstalling) {
        const auto& message=updateInfo_.message;
        MessageBoxW(window_?window_:controller_,message.c_str(),L"Обновления Viewery",MB_OK|
                    (updateInfo_.status==UpdateStatus::failed?MB_ICONWARNING:MB_ICONINFORMATION));
    } else if(updateInfo_.status==UpdateStatus::available) {
        NOTIFYICONDATAW tray{sizeof(tray)};tray.hWnd=controller_;tray.uID=1;tray.uFlags=NIF_INFO;
        wcscpy_s(tray.szInfoTitle,L"Обновление Viewery");
        const auto info=L"Доступна версия "+updateInfo_.version+L". Откройте меню Viewery для обновления.";
        wcsncpy_s(tray.szInfo,info.c_str(),_TRUNCATE);
        tray.dwInfoFlags=NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY,&tray);
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
    if(renameEnabled_) menuItem(menu,libraryOpenStudio,L"Переименование и коллекция");
    if (!tray) {
        menuItem(menu, static_cast<UINT>(Control::fullScreen), L"Полный экран\tF11");
        menuItem(menu, resetZoom, L"Вписать в окно\t0");
        menuItem(menu, static_cast<UINT>(Control::previous), L"Предыдущий файл\t←");
        menuItem(menu, static_cast<UINT>(Control::next), L"Следующий файл\t→");
    }
    menuItem(menu,openSettings,L"Настройки…");
    menuItem(menu, quit, L"Выйти из Viewery");
    const HWND owner = tray ? controller_ : window_;
    SetForegroundWindow(owner);
    const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                          point.x, point.y, 0, owner, nullptr));
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (picked) command(picked);
}

HMENU App::makeFileMenu(const fs::path& file) const
{
    HMENU menu=CreatePopupMenu();
    auto name=file.filename().wstring();
    if(name.size()>64) name=name.substr(0,61)+L"…";
    for(std::size_t at=0;(at=name.find(L'&',at))!=std::wstring::npos;at+=2) name.insert(at,1,L'&');
    AppendMenuW(menu,MF_STRING|MF_DISABLED,0,name.c_str());
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    menuItem(menu,filePreview,L"Открыть во Viewery");
    menuItem(menu,fileReveal,L"Показать в Проводнике");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    menuItem(menu,fileCopy,L"Копировать файл");
    menuItem(menu,fileCopyPath,L"Копировать путь");
    menuItem(menu,fileCopyName,L"Копировать имя");
    if(renameEnabled_) {
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        menuItem(menu,fileSelect,selectedFiles_.contains(file)?L"Убрать из выделения":L"Добавить в выделение");
        menuItem(menu,fileRename,selectedFiles_.contains(file)&&selectedFiles_.size()>1?
            L"Переименовать выбранные…":L"Переименовать файл…");
    }
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    menuItem(menu,fileProperties,L"Свойства");
    return menu;
}

void App::fileMenu(POINT point,const fs::path& file)
{
    const fs::path target=file;
    HMENU menu=makeFileMenu(target);
    const UINT picked=static_cast<UINT>(TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON|TPM_NONOTIFY,
        point.x,point.y,0,window_,nullptr));
    DestroyMenu(menu);
    if(picked) fileCommand(picked,target);
}

void App::fileCommand(UINT action,const fs::path& file)
{
    switch(action) {
    case filePreview:openMedia(file);syncStudio();return;
    case fileCopyPath:copyText(window_,file.wstring());return;
    case fileCopyName:copyText(window_,file.filename().wstring());return;
    case fileCopy:
        if(!copyFile(window_,file)) MessageBoxW(window_,L"Не удалось скопировать файл в буфер обмена.",L"Копирование",MB_OK|MB_ICONWARNING);
        return;
    case fileSelect:
        if(!selectedFiles_.erase(file)) selectedFiles_.insert(file);
        syncStudio();InvalidateRect(window_,&catalogRect_,FALSE);return;
    case fileRename:
        if(!selectedFiles_.contains(file)) {selectedFiles_.clear();selectedFiles_.insert(file);}
        openStudio();return;
    case fileProperties: {
        SHELLEXECUTEINFOW request{sizeof(request)};
        request.fMask=SEE_MASK_INVOKEIDLIST|SEE_MASK_FLAG_NO_UI;
        request.hwnd=window_;request.lpVerb=L"properties";request.lpFile=file.c_str();request.nShow=SW_SHOWNORMAL;
        if(!ShellExecuteExW(&request)) MessageBoxW(window_,L"Не удалось открыть свойства файла.",L"Свойства",MB_OK|MB_ICONWARNING);
        return;
    }
    case fileReveal: {
        const HRESULT result=revealInExplorer({file});
        if(FAILED(result)) MessageBoxW(window_,L"Не удалось показать файл в Проводнике.",L"Расположение файла",MB_OK|MB_ICONWARNING);
        return;
    }
    default:return;
    }
}

void App::artistMenu(POINT point)
{
    ensureStudio();
    if(!studio_) return;
    HMENU menu=CreatePopupMenu();
    menuItem(menu,artistsAll,L"Все файлы",collectionOptions_.group!=CollectionGroup::artist);
    menuItem(menu,artistsGrouped,L"Группы художников · по имени",collectionOptions_.group==CollectionGroup::artist&&collectionOptions_.filter.artist.empty());
    menuItem(menu,manageArtists,L"База художников…");
    CollectionViewOptions options;options.group=CollectionGroup::artist;options.sort=CollectionSort::artist;options.filenameArtists=true;
    std::vector<fs::path> paths;for(const auto& file:directoryFiles_) paths.push_back(file.path);
    const auto artists=buildCollectionView(studio_->snapshot(),paths,options);
    catalogFilterCommands_.clear();UINT action=10000;
    if(artists.artistCount) AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    for(const auto& row:artists.rows) if(row.group&&row.ancestorKeys.empty()&&!row.artistName.empty()&&row.artistName!=L"Без художника") {
        if(action>=10250) break;
        catalogFilterCommands_[action]={3,row.artistName};
        const auto caption=row.artistName+L"  ·  "+std::to_wstring(row.files);
        menuItem(menu,action++,caption.c_str(),collectionOptions_.filter.artist==row.artistName);
    }
    const UINT picked=static_cast<UINT>(TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON|TPM_NONOTIFY,point.x,point.y,0,window_,nullptr));
    DestroyMenu(menu);if(picked) command(picked);
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
    if(renameEnabled_) {
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        menuItem(menu,selectionAll,L"Выделить все\tCtrl+A");
        menuItem(menu,selectionClear,L"Снять выделение");
        menuItem(menu,selectionGroup,L"Выделить группу активного файла");
        menuItem(menu,libraryOpenStudio,L"Переименовать выбранные…");
        HMENU group=CreatePopupMenu();
        menuItem(group,groupNone,L"Без группировки",collectionOptions_.group==CollectionGroup::none);
        menuItem(group,groupArtist,L"Художник → работа → версии",collectionOptions_.group==CollectionGroup::artist);
        menuItem(group,groupWork,L"По работе",collectionOptions_.group==CollectionGroup::artwork);
        menuItem(group,groupFormat,L"По формату",collectionOptions_.group==CollectionGroup::format);
        menuItem(group,groupDirectory,L"По каталогу",collectionOptions_.group==CollectionGroup::directory);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(group),L"Группировка");
        HMENU metadataOrder=CreatePopupMenu();
        menuItem(metadataOrder,sortArtist,L"По художнику");menuItem(metadataOrder,sortNumber,L"По номеру работы");
        menuItem(metadataOrder,sortTitle,L"По названию работы");menuItem(metadataOrder,sortVersion,L"По версии");
        menuItem(metadataOrder,sortCustom,L"Пользовательский порядок");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(metadataOrder),L"Сортировка коллекции");
        HMENU filters=CreatePopupMenu();
        menuItem(filters,filterUnassigned,L"Без художника",collectionOptions_.filter.unassigned);
        menuItem(filters,filterUntitled,L"Без названия",collectionOptions_.filter.untitled);
        menuItem(filters,filterUnorganized,L"Не организовано",collectionOptions_.filter.unorganized);
        catalogFilterCommands_.clear();
        UINT action=10000;
        const auto filterItems=[&](const wchar_t* title,int type,const std::set<std::wstring>& values) {
            HMENU sub=CreatePopupMenu();
            catalogFilterCommands_[action]={type,L""};menuItem(sub,action++,L"Все");
            for(const auto& value:values) {
                if(action>=10250) break;
                catalogFilterCommands_[action]={type,value};menuItem(sub,action++,value.c_str());
            }
            AppendMenuW(filters,MF_POPUP,reinterpret_cast<UINT_PTR>(sub),title);
        };
        std::set<std::wstring> formats,versions,artists;
        for(const auto& file:directoryFiles_) formats.insert(file.extension);
        if(studio_) {
            for(const auto& version:studio_->snapshot().versions) versions.insert(std::to_wstring(version.number));
            for(const auto& artist:studio_->snapshot().artists) artists.insert(artist.name);
        }
        filterItems(L"Формат",0,formats);filterItems(L"Версия",1,versions);filterItems(L"Художник",2,artists);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(filters),L"Фильтр");
        HMENU keys=CreatePopupMenu();
        menuItem(keys,groupKeysForward,L"Alt+↑ / ↓",!groupChordControl_);
        menuItem(keys,groupKeysReverse,L"Ctrl+Alt+↑ / ↓",groupChordControl_);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(keys),L"Навигация группами");
        menuItem(menu,libraryRefresh,L"Обновить коллекцию");
    }
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
    if(const auto action=catalogFilterCommands_.find(id);action!=catalogFilterCommands_.end()) {
        if(!studio_) ensureStudio();
        auto& filter=collectionOptions_.filter;
        if(action->second.first==0) filter.format=action->second.second;
        else if(action->second.first==1) filter.version=action->second.second;
        else filter.artist=action->second.second;
        if(action->second.first==3) {collectionOptions_.group=CollectionGroup::artist;collectionOptions_.sort=CollectionSort::artist;}
        rebuildCollectionView();scroll_=0;layout();return;
    }
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
    case static_cast<UINT>(Control::renameStudio):
        if(detail_==Detail::renameStudio) closeStudio();else openStudio();
        return;
    case libraryOpenStudio:openStudio();return;
    case openSettings:case static_cast<UINT>(Control::settings):openSettingsWindow();return;
    case openTemplates:
        if(renameEnabled_) {ensureStudio();studio_->openTemplates();}
        return;
    case static_cast<UINT>(Control::catalogSelect):command(selectionMode);return;
    case static_cast<UINT>(Control::catalogArtists):artistMenu(controlPoint(Control::catalogArtists));return;
    case static_cast<UINT>(Control::catalogReveal):revealCatalogSelection();return;
    case manageArtists:openArtistRegistry();return;
    case artistsAll:
        ensureStudio();collectionOptions_.filter={};collectionOptions_.group=CollectionGroup::none;collectionOptions_.sort=CollectionSort::name;
        SetWindowTextW(catalogSearch_,L"");rebuildCollectionView();scroll_=0;savePreferences();layout();return;
    case artistsGrouped:
        ensureStudio();collectionOptions_.filter.artist.clear();collectionOptions_.group=CollectionGroup::artist;collectionOptions_.sort=CollectionSort::artist;
        rebuildCollectionView();scroll_=0;savePreferences();layout();return;
    case RenameStudio::closeCommandId:closeStudio();return;
    case selectionMode:
        selectMode_=!selectMode_;selectedFiles_.clear();selectionAnchor_=-1;
        syncStudio();layout();return;
    case selectionClear:selectedFiles_.clear();syncStudio();layout();return;
    case selectionAll:
        if(renameEnabled_) for(const auto& file:files_) selectedFiles_.insert(file.path);
        syncStudio();layout();return;
    case selectionGroup: {
        if(!studio_) {openStudio();return;}
        std::vector<std::wstring> ancestors;
        for(const auto& row:collectionView_.rows) if(!row.group&&samePath(row.path,path_)) {
            ancestors=row.ancestorKeys;break;
        }
        for(const auto& row:collectionView_.rows) if(!row.group&&
            (ancestors.empty()||(!row.ancestorKeys.empty()&&row.ancestorKeys.front()==ancestors.front()))) {
            selectedFiles_.insert(row.path);
        }
        syncStudio();layout();return;
    }
    case moduleRename: {
        renameEnabled_=!renameEnabled_;
        const DWORD enabled=renameEnabled_?1:0;
        RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Viewery",L"RenameStudioEnabled",REG_DWORD,&enabled,sizeof(enabled));
        if(!renameEnabled_) {
            if(artists_) artists_->hide();
            detail_=Detail::none;selectedFiles_.clear();selectMode_=false;
            if(studio_&&studio_->templatesWindow()) SendMessageW(studio_->templatesWindow(),WM_CLOSE,0,0);
            if(studio_&&studio_->busy()) {studio_->cancel();studio_->hide();disableStudioPending_=true;}
            else if(studio_&&studio_->queuedFileCount()) studio_->hide();
            else studio_.reset();
            files_=directoryFiles_;sortCatalog(files_,sort_);
        } else syncStudio();
        layout();return;
    }
    case groupNone:case groupArtist:case groupWork:case groupFormat:case groupDirectory:
        if(!studio_) ensureStudio();
        collectionOptions_.group=static_cast<CollectionGroup>(id-groupNone);
        rebuildCollectionView();scroll_=0;savePreferences();layout();return;
    case sortArtist:case sortNumber:case sortTitle:case sortVersion:case sortCustom:
        if(!studio_) ensureStudio();
        collectionOptions_.sort=id==sortArtist?CollectionSort::artist:id==sortNumber?CollectionSort::number:
            id==sortTitle?CollectionSort::title:id==sortVersion?CollectionSort::version:CollectionSort::custom;
        rebuildCollectionView();savePreferences();layout();return;
    case filterUnassigned:case filterUntitled:case filterUnorganized:
        if(!studio_) ensureStudio();
        if(id==filterUnassigned) collectionOptions_.filter.unassigned=!collectionOptions_.filter.unassigned;
        if(id==filterUntitled) collectionOptions_.filter.untitled=!collectionOptions_.filter.untitled;
        if(id==filterUnorganized) collectionOptions_.filter.unorganized=!collectionOptions_.filter.unorganized;
        rebuildCollectionView();scroll_=0;layout();return;
    case libraryRefresh:
        if(!studio_) ensureStudio();
        else {studio_->setFiles({},{});syncStudio();}
        return;
    case groupKeysForward:case groupKeysReverse:
        groupChordControl_=id==groupKeysReverse;savePreferences();return;
    case updatesAutomatic:
        UpdateService::setAutomaticChecks(!UpdateService::automaticChecks());return;
    case updatesCheck:if(updates_) updates_->check(true);return;
    case mediaToolsInstall:if(updates_) updates_->installMediaTools();return;
    case updatesNotes:
        if(updateInfo_.notes.empty()) return;
        MessageBoxW(window_?window_:controller_,updateInfo_.notes.c_str(),L"Что нового",MB_OK);return;
    case updatesInstall:
        if(studio_&&studio_->queuedFileCount()) {
            MessageBoxW(window_,L"Сначала переименуйте подготовленные файлы или очистите очередь. Обновление перезапускает приложение.",L"Очередь переименования",MB_OK);return;
        }
        if(exportThread_.joinable()||(studio_&&studio_->busy())||(artists_&&artists_->busy())) {
            MessageBoxW(window_,L"Завершите экспорт или обработку коллекции перед обновлением.",L"Обновление",MB_OK);return;
        }
        if(MessageBoxW(window_?window_:controller_,L"Установить обновление Viewery? Приложение будет закрыто для обновления. После установки запустите его снова.",
                       L"Обновление",MB_YESNO|MB_ICONQUESTION)==IDYES&&updates_) updates_->install();
        return;
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
    case nameSort: sort_ = CatalogSort::name;collectionOptions_.sort=CollectionSort::name;break;
    case dateSort: sort_ = CatalogSort::newest;collectionOptions_.sort=CollectionSort::date;break;
    case sizeSort: sort_ = CatalogSort::size;collectionOptions_.sort=CollectionSort::size;break;
    case typeSort: sort_ = CatalogSort::type;collectionOptions_.sort=CollectionSort::extension;break;
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
    case quit:
        if(artists_&&artists_->busy()) {
            MessageBoxW(window_?window_:controller_,L"Дождитесь завершения операции с художником.",L"База художников",MB_OK);return;
        }
        if(studio_&&studio_->queuedFileCount()&&MessageBoxW(window_?window_:controller_,
            L"В очереди есть подготовленные имена. Завершить Viewery и очистить очередь?",L"Очередь переименования",
            MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
        PostQuitMessage(0);return;
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
    if(studio_) rebuildCollectionView();
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
    DWORD rightWidth=510,rightBytes=sizeof(rightWidth);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Viewery",L"RightPanelWidth",RRF_RT_REG_DWORD,nullptr,&rightWidth,&rightBytes)==ERROR_SUCCESS)
        detailsWidth_=std::clamp(static_cast<int>(rightWidth),360,900);
    DWORD view[3]{0,static_cast<DWORD>(CollectionSort::name),0};
    DWORD viewBytes=sizeof(view);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Viewery",L"CollectionView",RRF_RT_REG_BINARY,
                    nullptr,view,&viewBytes)==ERROR_SUCCESS&&viewBytes==sizeof(view)) {
        if(view[0]<=static_cast<DWORD>(CollectionGroup::directory)) collectionOptions_.group=static_cast<CollectionGroup>(view[0]);
        if(view[1]<=static_cast<DWORD>(CollectionSort::custom)) collectionOptions_.sort=static_cast<CollectionSort>(view[1]);
        groupChordControl_=view[2]!=0;
    }
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
    const DWORD rightWidth=static_cast<DWORD>(detailsWidth_);
    RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Viewery",L"RightPanelWidth",REG_DWORD,&rightWidth,sizeof(rightWidth));
    const DWORD view[3]{static_cast<DWORD>(collectionOptions_.group),static_cast<DWORD>(collectionOptions_.sort),
                        groupChordControl_?1U:0U};
    RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\Viewery",L"CollectionView",REG_BINARY,view,sizeof(view));
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
        if(message==startupUpdateMessage) {updates_->check(false);return 0;}
        if(message==updateMessage) {acceptUpdate();return 0;}
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
    case WM_SETCURSOR: {
        if(LOWORD(lparam)==HTCLIENT) {
            POINT point{};GetCursorPos(&point);ScreenToClient(window_,&point);
            if(PtInRect(&detailsDivider_,point)||PtInRect(&divider_,point)) {
                SetCursor(LoadCursorW(nullptr,PtInRect(&detailsDivider_,point)||panel_==Panel::left?IDC_SIZEWE:IDC_SIZENS));return TRUE;
            }
        }
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_DRAWITEM:
        if (wparam >= static_cast<UINT>(Control::open) &&
            wparam <= static_cast<UINT>(Control::catalogReveal)) {
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
    case WM_VSCROLL:case WM_HSCROLL:
        if(reinterpret_cast<HWND>(lparam)==catalogScroll_) {
            const bool horizontal=panel_==Panel::bottom;
            const int page=horizontal?spanX(catalogViewport()):spanY(catalogViewport());
            int target=scroll_;
            switch(LOWORD(wparam)) {
            case SB_LINEUP:target-=dp(42);break;
            case SB_LINEDOWN:target+=dp(42);break;
            case SB_PAGEUP:target-=page;break;
            case SB_PAGEDOWN:target+=page;break;
            case SB_TOP:target=0;break;
            case SB_BOTTOM:target=maxScroll();break;
            case SB_THUMBPOSITION:case SB_THUMBTRACK: {
                SCROLLINFO info{sizeof(info),SIF_TRACKPOS};GetScrollInfo(catalogScroll_,SB_CTL,&info);
                target=info.nTrackPos;break;
            }
            }
            scrollCatalog(std::clamp(target,0,maxScroll())-scroll_);return 0;
        }
        if (reinterpret_cast<HWND>(lparam) == volumeTrack_) {
            setVolume(static_cast<double>(SendMessageW(volumeTrack_, TBM_GETPOS, 0, 0)));
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if(reinterpret_cast<HWND>(lparam)==rangeStart_||reinterpret_cast<HWND>(lparam)==rangeEnd_||
           reinterpret_cast<HWND>(lparam)==catalogSearch_) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, textColor);
            SetBkColor(dc, raised);
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        break;
    case WM_COMMAND:
        if(LOWORD(wparam)==RenameStudio::closeCommandId) {closeStudio();return 0;}
        if(LOWORD(wparam)==RenameStudio::artistsCommandId) {openArtistRegistry();return 0;}
        if(LOWORD(wparam)==catalogSearchId&&HIWORD(wparam)==EN_CHANGE&&studio_) {
            std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(catalogSearch_))+1,L'\0');
            value.resize(static_cast<std::size_t>(GetWindowTextW(catalogSearch_,value.data(),static_cast<int>(value.size()))));
            collectionOptions_.filter.text=std::move(value);
            rebuildCollectionView();scroll_=0;layout();return 0;
        }
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
            LOWORD(wparam) <= static_cast<UINT>(Control::catalogReveal) && HIWORD(wparam) == BN_CLICKED) {
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
    case libraryMessage:acceptLibraryChanges();return 0;
    case artistMessage:acceptArtistChanges();return 0;
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
        if(resizingDetails_) {
            detailsWidth_=std::clamp(MulDiv(client_.right-point.x,96,static_cast<int>(dpi_)),360,900);
            layout();
        } else if (resizingPanel_) {
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
        if(resizingPanel_||resizingDetails_) savePreferences();
        if(resizingPanel_||resizingDetails_||scrubbing_) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        gesture_ = CanvasGesture::none;
        doubleClick_ = resizingPanel_ = resizingDetails_ = scrubbing_ = false;
        return 0;
    case WM_CONTEXTMENU: {
        const HWND camera = buttons_[static_cast<UINT>(Control::capture) - static_cast<UINT>(Control::open)];
        if (reinterpret_cast<HWND>(wparam) == camera) {
            captureFrame(FrameCapturePurpose::save);
            return 0;
        }
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const bool keyboard=point.x==-1&&point.y==-1;
        if(keyboard&&current_>=0&&current_<static_cast<int>(files_.size())&&!IsRectEmpty(&catalogRect_)) {
            const auto item=itemRect(static_cast<std::size_t>(current_));
            point={item.left+spanX(item)/2,item.top+spanY(item)/2};
            RECT viewport=catalogRect_;viewport.top+=catalogHeaderHeight();
            if(PtInRect(&viewport,point)) {
                ClientToScreen(window_,&point);
                fileMenu(point,files_[static_cast<std::size_t>(current_)].path);return 0;
            }
        }
        if(keyboard) GetCursorPos(&point);
        POINT local = point;
        ScreenToClient(window, &local);
        if(const int index=catalogItemAt(local);index>=0) fileMenu(point,files_[static_cast<std::size_t>(index)].path);
        else if(PtInRect(&catalogRect_, local)) catalogMenu(point);
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
        if(artists_&&artists_->processMessage(event)) continue;
        if(settings_&&settings_->processMessage(event)) continue;
        if(studio_&&studio_->processMessage(event)) continue;
        const bool studioInput=studio_&&(event.hwnd==studio_->window()||IsChild(studio_->window(),event.hwnd));
        if(studioInput&&event.message==WM_KEYDOWN&&event.wParam==VK_ESCAPE) {
            wchar_t className[32]{};GetClassNameW(event.hwnd,className,32);
            HWND combo=nullptr;
            if(wcscmp(className,L"ComboBox")==0) combo=event.hwnd;
            else if(HWND parent=GetParent(event.hwnd);parent) {
                GetClassNameW(parent,className,32);
                if(wcscmp(className,L"ComboBox")==0) combo=parent;
            }
            if(!combo||!SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)) {closeStudio();continue;}
        }
        if(studioInput&&event.message==WM_KEYDOWN&&event.wParam==VK_TAB&&IsDialogMessageW(studio_->window(),&event)) continue;
        if (window_ && IsChild(window_, event.hwnd) && event.hwnd != rangeStart_ && event.hwnd != rangeEnd_ &&
            event.hwnd!=catalogSearch_&&!studioInput&&
            !(event.hwnd == volumeTrack_ && event.wParam >= VK_PRIOR && event.wParam <= VK_DOWN) &&
            (event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN) && event.wParam != VK_TAB) {
            if (event.wParam == VK_SPACE && (event.lParam & (1LL << 30))) continue;
            if (key(event.wParam, event.message == WM_SYSKEYDOWN && (event.lParam & (1LL << 29)))) continue;
        }
        if (!studioInput&&window_ && IsWindowVisible(window_) && (event.message == WM_KEYDOWN && event.wParam == VK_TAB) &&
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
    bool checkUpdates = false;
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
        else if(arg==L"--check-updates") result.checkUpdates=true;
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
    if(launch.checkUpdates) {
        // Run the production updater in this exact executable without touching
        // the existing viewer, preferences, tray icon or Explorer hooks.
        UpdateService updates(nullptr,0);
        updates.check(true);
        while(updates.busy()) Sleep(25);
        const auto result=updates.take();
        const std::wstring report=L"Viewery " L"" VIEWERY_VERSION L"\r\n"+result.message+L"\r\n";
        const int count=WideCharToMultiByte(CP_UTF8,0,report.data(),static_cast<int>(report.size()),nullptr,0,nullptr,nullptr);
        std::string bytes(static_cast<std::size_t>(std::max(0,count)),'\0');
        if(count>0) WideCharToMultiByte(CP_UTF8,0,report.data(),static_cast<int>(report.size()),bytes.data(),count,nullptr,nullptr);
        DWORD written=0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
        return result.status==UpdateStatus::failed?1:0;
    }
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
