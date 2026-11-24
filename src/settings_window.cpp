#include "settings_window.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace viewery {
namespace {

constexpr COLORREF background=RGB(23,26,33), input=RGB(36,40,52);
constexpr COLORREF foreground=RGB(241,242,247), muted=RGB(149,155,170), accent=RGB(155,140,255);
enum Control : int {
    tabGeneral=2200,tabControls,tabUpdates,tabAbout,closeButton,
    renameCheck,templatesButton,backupButton,exportButton,importButton,
    quickChoice,groupChoice,automaticCheck,checkButton,installButton,notesButton,
    mediaButton,statusText,versionText,repositoryButton,
    collectionLabel,backupLabel,quickLabel,groupLabel,keyboardHint,updatesLabel,
    aboutTitle,aboutText,licenseText,viewportId,contentId,artistsButton
};

void fill(HDC dc,RECT rect,COLORREF color)
{
    HBRUSH brush=CreateSolidBrush(color);
    FillRect(dc,&rect,brush);
    DeleteObject(brush);
}

void setText(HWND window,const std::wstring& value)
{
    const int size=GetWindowTextLengthW(window);
    std::wstring previous(static_cast<std::size_t>(size)+1,L'\0');
    previous.resize(static_cast<std::size_t>(GetWindowTextW(window,previous.data(),size+1)));
    if(previous!=value) SetWindowTextW(window,value.c_str());
}

std::wstring windowText(HWND window)
{
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window))+1,L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(window,value.data(),static_cast<int>(value.size()))));
    return value;
}

std::wstring wide(const char* value)
{
    const int count=MultiByteToWideChar(CP_UTF8,0,value,-1,nullptr,0);
    if(count<=0) return L"Не удалось выполнить действие.";
    std::wstring output(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value,-1,output.data(),count);
    output.resize(static_cast<std::size_t>(count-1));
    return output;
}

} // namespace

struct SettingsWindow::Impl {
    HWND owner=nullptr,window=nullptr,viewport=nullptr,content=nullptr;
    Callback callback;
    SettingsState state;
    std::map<int,HWND> controls;
    HFONT font=nullptr,smallFont=nullptr,titleFont=nullptr;
    HBRUSH backgroundBrush=CreateSolidBrush(background),inputBrush=CreateSolidBrush(input);
    UINT dpi=96;
    int tab=0,scrollOffset=0,scrollLimit=0;
    bool filling=false,layingOut=false;

    int dp(int value) const {return MulDiv(value,static_cast<int>(dpi),96);}
    HWND c(int id) const {const auto found=controls.find(id);return found==controls.end()?nullptr:found->second;}

    Impl(HWND parent,Callback action):owner(parent),callback(std::move(action))
    {
        try {
            const INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES};
            if(!InitCommonControlsEx(&common)) throw std::runtime_error("Cannot initialize settings controls");
            static const ATOM klass=[] {
                WNDCLASSW description{};
                description.hInstance=GetModuleHandleW(nullptr);
                description.lpfnWndProc=procedure;
                description.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                description.lpszClassName=L"Viewery.Settings.1";
                return RegisterClassW(&description);
            }();
            if(!klass) throw std::runtime_error("Cannot register settings window");
            if(owner) dpi=GetDpiForWindow(owner);
            if(!dpi) dpi=96;
            window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,L"Viewery.Settings.1",L"Настройки",
                WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_CLIPCHILDREN,
                CW_USEDEFAULT,CW_USEDEFAULT,dp(500),dp(640),owner,nullptr,GetModuleHandleW(nullptr),this);
            if(!window) throw std::runtime_error("Cannot create settings window");
            const BOOL dark=TRUE;
            DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
            viewport=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.Settings.1",L"",
                WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|WS_VSCROLL,
                0,0,0,0,window,reinterpret_cast<HMENU>(viewportId),GetModuleHandleW(nullptr),this);
            content=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.Settings.1",L"",
                WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
                0,0,0,0,viewport,reinterpret_cast<HMENU>(contentId),GetModuleHandleW(nullptr),this);
            if(!viewport||!content) throw std::runtime_error("Cannot create settings viewport");
            build();
            makeFonts();
            applyState();
            layout();
        } catch(...) {
            if(window) DestroyWindow(window);
            DeleteObject(font);DeleteObject(smallFont);DeleteObject(titleFont);
            DeleteObject(backgroundBrush);DeleteObject(inputBrush);
            throw;
        }
    }

    ~Impl()
    {
        if(window) DestroyWindow(window);
        DeleteObject(font);DeleteObject(smallFont);DeleteObject(titleFont);
        DeleteObject(backgroundBrush);DeleteObject(inputBrush);
    }

    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l)
    {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self) return DefWindowProcW(hwnd,message,w,l);
        try {return self->dispatch(hwnd,message,w,l);}
        catch(const std::exception& error) {
            MessageBoxW(self->window,wide(error.what()).c_str(),L"Настройки",MB_OK|MB_ICONWARNING);
            return 0;
        }
    }

    static LRESULT CALLBACK inputProc(HWND child,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR value)
    {
        auto& self=*reinterpret_cast<Impl*>(value);
        if(message==WM_KEYDOWN&&w==VK_ESCAPE&&!self.dropdownOpen()) {self.hide();return 0;}
        if(message==WM_MOUSEWHEEL&&!self.dropdownOpen()) {
            return SendMessageW(self.viewport,message,w,l);
        }
        if((message==WM_PAINT||message==WM_PRINTCLIENT)&&
            (GetDlgCtrlID(child)==renameCheck||GetDlgCtrlID(child)==automaticCheck)) {
            if(message==WM_PRINTCLIENT) {self.drawCheck(child,reinterpret_cast<HDC>(w));return 0;}
            PAINTSTRUCT paint{};HDC dc=BeginPaint(child,&paint);self.drawCheck(child,dc);EndPaint(child,&paint);return 0;
        }
        if(message==WM_ERASEBKGND&&(GetDlgCtrlID(child)==renameCheck||GetDlgCtrlID(child)==automaticCheck)) return 1;
        return DefSubclassProc(child,message,w,l);
    }

    HWND create(int id,const wchar_t* type,const wchar_t* text,DWORD style,bool fixed=false)
    {
        const DWORD flags=WS_CHILD|WS_CLIPSIBLINGS|style|(wcscmp(type,L"STATIC")==0?0:WS_TABSTOP);
        HWND result=CreateWindowExW(0,type,text,flags,
            0,0,0,0,fixed?window:content,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!result) throw std::runtime_error("Cannot create settings control");
        controls.emplace(id,result);
        SetWindowTheme(result,L"DarkMode_Explorer",nullptr);
        SetWindowSubclass(result,inputProc,1,reinterpret_cast<DWORD_PTR>(this));
        return result;
    }
    void button(int id,const wchar_t* text,bool fixed=false) {create(id,L"BUTTON",text,BS_OWNERDRAW,fixed);}
    void label(int id,const wchar_t* text) {create(id,L"STATIC",text,SS_LEFT|SS_NOPREFIX);}
    void combo(int id) {create(id,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_HASSTRINGS|CBS_OWNERDRAWFIXED|WS_VSCROLL);}
    void item(int id,const wchar_t* text) {SendMessageW(c(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));}

    void build()
    {
        button(tabGeneral,L"Общие",true);button(tabControls,L"Клавиши",true);
        button(tabUpdates,L"Обновления",true);button(tabAbout,L"О Viewery",true);
        button(closeButton,L"Закрыть",true);
        label(collectionLabel,L"Коллекция");
        create(renameCheck,L"BUTTON",L"Переименование и коллекция",BS_AUTOCHECKBOX);
        button(templatesButton,L"Редактор шаблонов…");
        button(artistsButton,L"База художников…");
        label(backupLabel,L"Резервная копия и обмен");
        button(backupButton,L"Копия базы");button(exportButton,L"Экспорт JSON");button(importButton,L"Импорт JSON");
        label(quickLabel,L"Quick View в Проводнике");
        combo(quickChoice);item(quickChoice,L"Пробел");item(quickChoice,L"Ctrl + Пробел");
        item(quickChoice,L"Ctrl + Shift + Пробел");item(quickChoice,L"Отключено");
        label(groupLabel,L"Переход между художниками");
        combo(groupChoice);item(groupChoice,L"Alt + ↑ / ↓");item(groupChoice,L"Ctrl + Alt + ↑ / ↓");
        label(keyboardHint,L"← / → — предыдущий / следующий файл\n"
            L"↑ / ↓ — строки или файлы каталога\n"
            L"Alt + ← / → — кадр или панорамирование\n"
            L"Shift + ← / → — перемотка на 5 секунд\n"
            L"Ctrl + колесо — масштаб\n"
            L"Колесо над динамиком — громкость\n\n"
            L"Обычное окно:\n"
            L"ЛКМ — панорамирование\n"
            L"Ctrl + ЛКМ — перемещение окна\n\n"
            L"Миниатюра:\n"
            L"ЛКМ — перемещение окна\n"
            L"Ctrl + ЛКМ — панорамирование\n"
            L"Пробел — пауза\n"
            L"Двойной пробел — закрыть");
        create(automaticCheck,L"BUTTON",L"Проверять обновления при запуске",BS_AUTOCHECKBOX);
        label(updatesLabel,L"Обновления приложения");
        label(statusText,L"Проверка обновлений ещё не выполнялась.");
        button(checkButton,L"Проверить");button(installButton,L"Установить");button(notesButton,L"Что нового");
        button(mediaButton,L"Установить медиакомпоненты");
        label(aboutTitle,L"Viewery");label(versionText,L"");
        label(aboutText,L"Фото, анимация и видео.\nЛокальная коллекция и переименование.");
        label(licenseText,L"Собственный код: лицензия MIT.");
        button(repositoryButton,L"Исходный код и выпуски");
    }

    void makeFonts()
    {
        HFONT old=font,oldSmall=smallFont,oldTitle=titleFont;
        const auto make=[&](int size,int weight) {
            return CreateFontW(-dp(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        };
        font=make(14,FW_NORMAL);smallFont=make(12,FW_NORMAL);titleFont=make(22,FW_SEMIBOLD);
        for(const auto& [id,control]:controls) {
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(id==aboutTitle?titleFont:
                id==keyboardHint||(id>=tabGeneral&&id<=tabAbout)?smallFont:font),FALSE);
        }
        for(int id:{quickChoice,groupChoice}) {
            SendMessageW(c(id),CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),dp(27));
            SendMessageW(c(id),CB_SETITEMHEIGHT,0,dp(27));
        }
        DeleteObject(old);DeleteObject(oldSmall);DeleteObject(oldTitle);
    }

    bool dropdownOpen() const
    {
        return SendMessageW(c(quickChoice),CB_GETDROPPEDSTATE,0,0)||SendMessageW(c(groupChoice),CB_GETDROPPEDSTATE,0,0);
    }

    void hide()
    {
        for(int id:{quickChoice,groupChoice}) SendMessageW(c(id),CB_SHOWDROPDOWN,FALSE,0);
        ShowWindow(window,SW_HIDE);
    }

    void applyState()
    {
        filling=true;
        const auto check=[&](int id,bool enabled) {
            if((SendMessageW(c(id),BM_GETCHECK,0,0)==BST_CHECKED)!=enabled) {
                SendMessageW(c(id),BM_SETCHECK,enabled?BST_CHECKED:BST_UNCHECKED,0);
                InvalidateRect(c(id),nullptr,FALSE);
            }
        };
        check(renameCheck,state.renameEnabled);check(automaticCheck,state.automaticUpdates);
        if(SendMessageW(c(quickChoice),CB_GETCURSEL,0,0)!=static_cast<LRESULT>(state.quickHotkey)) {
            SendMessageW(c(quickChoice),CB_SETCURSEL,static_cast<WPARAM>(state.quickHotkey),0);
        }
        if(SendMessageW(c(groupChoice),CB_GETCURSEL,0,0)!=(state.groupControlChord?1:0)) {
            SendMessageW(c(groupChoice),CB_SETCURSEL,state.groupControlChord?1:0,0);
        }
        setText(c(statusText),state.updateStatus.empty()?L"Проверка обновлений ещё не выполнялась.":state.updateStatus);
        setText(c(versionText),L"Версия "+state.version);
        for(int id:{templatesButton,artistsButton,backupButton,exportButton,importButton}) EnableWindow(c(id),state.renameEnabled);
        EnableWindow(c(checkButton),!state.updateBusy);
        EnableWindow(c(mediaButton),!state.updateBusy);
        EnableWindow(c(installButton),state.canInstall&&!state.updateBusy);
        EnableWindow(c(notesButton),state.canShowReleaseNotes&&!state.updateBusy);
        filling=false;
    }

    void position(HWND control,int x,int y,int width,int height)
    {
        RECT previous{};GetWindowRect(control,&previous);
        MapWindowPoints(nullptr,GetParent(control),reinterpret_cast<POINT*>(&previous),2);
        const RECT next{dp(x),dp(y),dp(x+std::max(1,width)),dp(y+std::max(1,height))};
        if(!EqualRect(&previous,&next)) SetWindowPos(control,nullptr,next.left,next.top,next.right-next.left,next.bottom-next.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
    }

    void layout()
    {
        if(!viewport||!content||layingOut||dropdownOpen()) return;
        layingOut=true;
        RECT bounds{};GetClientRect(window,&bounds);
        const int width=MulDiv(bounds.right,96,static_cast<int>(dpi));
        const int height=MulDiv(bounds.bottom,96,static_cast<int>(dpi));
        const int viewHeight=std::max(1,height-102);
        position(viewport,0,55,width,viewHeight);
        int hintHeight=260;
        if(tab==1) {
            RECT textRect{0,0,std::max(1,static_cast<int>(bounds.right)-dp(45)),0};
            HDC dc=GetDC(c(keyboardHint));
            const auto oldFont=SelectObject(dc,smallFont);
            const auto value=windowText(c(keyboardHint));
            DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&textRect,
                DT_CALCRECT|DT_WORDBREAK|DT_EXTERNALLEADING|DT_NOPREFIX);
            SelectObject(dc,oldFont);ReleaseDC(c(keyboardHint),dc);
            hintHeight=std::max(260,(static_cast<int>(textRect.bottom)*96+static_cast<int>(dpi)-1)/static_cast<int>(dpi)+8);
        }
        const std::array pageHeights{365,185+hintHeight+16,340,280};
        const int formHeight=std::max(viewHeight,pageHeights[static_cast<std::size_t>(tab)]);
        scrollLimit=std::max(0,formHeight-viewHeight);
        scrollOffset=std::clamp(scrollOffset,0,scrollLimit);
        SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
        scroll.nMin=0;scroll.nMax=formHeight-1;scroll.nPage=static_cast<UINT>(viewHeight);scroll.nPos=scrollOffset;
        SetScrollInfo(viewport,SB_VERT,&scroll,TRUE);
        RECT body{};GetClientRect(viewport,&body);
        const int bodyWidth=MulDiv(body.right,96,static_cast<int>(dpi));
        position(content,0,-scrollOffset,bodyWidth,formHeight);
        std::set<int> visible;
        const auto put=[&](int id,int x,int y,int w,int h=32) {
            visible.insert(id);position(c(id),x,y,w,h);
        };
        const int tabWidth=std::max(1,(width-36)/4);
        for(int index=0;index<4;++index) put(tabGeneral+index,12+index*(tabWidth+4),12,tabWidth,31);
        put(closeButton,std::max(12,width-118),std::max(0,height-39),106,30);
        const int inside=std::max(1,bodyWidth-28);
        if(tab==0) {
            put(collectionLabel,14,12,inside,24);
            put(renameCheck,14,46,inside,31);
            put(templatesButton,14,91,inside,34);
            put(artistsButton,14,135,inside,34);
            put(backupLabel,14,198,inside,23);
            const int half=(inside-10)/2;
            put(backupButton,14,234,half);put(exportButton,24+half,234,half);
            put(importButton,14,276,inside);
        } else if(tab==1) {
            put(quickLabel,14,12,inside,23);put(quickChoice,14,44,inside,190);
            put(groupLabel,14,96,inside,23);put(groupChoice,14,128,inside,130);
            put(keyboardHint,14,185,inside,hintHeight);
        } else if(tab==2) {
            put(updatesLabel,14,12,inside,24);put(automaticCheck,14,47,inside,32);
            put(statusText,14,96,inside,74);
            const int half=(inside-10)/2;
            put(checkButton,14,185,half);put(installButton,24+half,185,half);
            put(notesButton,14,227,inside);put(mediaButton,14,281,inside,34);
        } else {
            put(aboutTitle,14,12,inside,36);put(versionText,14,60,inside,24);
            put(aboutText,14,106,inside,55);put(licenseText,14,175,inside,23);
            put(repositoryButton,14,222,inside,34);
        }
        for(const auto& [id,control]:controls) {
            const bool shown=(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE)!=0;
            if(visible.contains(id)!=shown) ShowWindow(control,visible.contains(id)?SW_SHOW:SW_HIDE);
        }
        layingOut=false;
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }

    void scrollTo(int value)
    {
        if(dropdownOpen()) return;
        const int next=std::clamp(value,0,scrollLimit);
        if(next==scrollOffset) return;
        scrollOffset=next;
        SetScrollPos(viewport,SB_VERT,scrollOffset,TRUE);
        SetWindowPos(content,nullptr,0,-dp(scrollOffset),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        RedrawWindow(viewport,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }

    void drawCheck(HWND control,HDC dc)
    {
        RECT bounds{};GetClientRect(control,&bounds);fill(dc,bounds,background);
        const int top=(bounds.bottom-dp(18))/2;
        RECT check{0,top,dp(18),top+dp(18)};
        const bool checked=SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
        fill(dc,check,checked?accent:input);
        if(!checked) {HBRUSH edge=CreateSolidBrush(muted);FrameRect(dc,&check,edge);DeleteObject(edge);}
        HFONT old=static_cast<HFONT>(SelectObject(dc,font));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,background);
        if(checked) {auto mark=check;DrawTextW(dc,L"✓",1,&mark,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);}
        RECT caption=bounds;caption.left=dp(28);
        SetTextColor(dc,IsWindowEnabled(control)?foreground:muted);
        const auto value=windowText(control);
        DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&caption,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,old);
        if(GetFocus()==control) {InflateRect(&caption,-1,-3);DrawFocusRect(dc,&caption);}
    }

    void emit(SettingsAction action,int value=0)
    {
        if(callback) callback(SettingsEvent{action,value});
    }

    void command(int id,int notification)
    {
        if(filling) return;
        if(id>=tabGeneral&&id<=tabAbout&&notification==BN_CLICKED) {
            tab=id-tabGeneral;scrollOffset=0;layout();return;
        }
        if(notification==CBN_CLOSEUP) {PostMessageW(window,WM_APP+1,0,0);return;}
        if(id==quickChoice&&notification==CBN_SELCHANGE) {
            const auto selected=SendMessageW(c(id),CB_GETCURSEL,0,0);
            if(selected>=0&&selected<=3) {state.quickHotkey=static_cast<PreviewHotkey>(selected);emit(SettingsAction::quickHotkey,static_cast<int>(selected));}
            return;
        }
        if(id==groupChoice&&notification==CBN_SELCHANGE) {
            state.groupControlChord=SendMessageW(c(id),CB_GETCURSEL,0,0)==1;
            emit(SettingsAction::groupChord,state.groupControlChord?1:0);return;
        }
        if(notification!=BN_CLICKED) return;
        if(!IsWindowEnabled(c(id))) return;
        switch(id) {
        case closeButton:hide();return;
        case renameCheck:
            state.renameEnabled=SendMessageW(c(id),BM_GETCHECK,0,0)==BST_CHECKED;
            emit(SettingsAction::renameModule,state.renameEnabled?1:0);applyState();return;
        case automaticCheck:
            state.automaticUpdates=SendMessageW(c(id),BM_GETCHECK,0,0)==BST_CHECKED;
            emit(SettingsAction::automaticUpdates,state.automaticUpdates?1:0);return;
        case templatesButton:emit(SettingsAction::templates);return;
        case artistsButton:emit(SettingsAction::artists);return;
        case backupButton:emit(SettingsAction::backupCollection);return;
        case exportButton:emit(SettingsAction::exportMetadata);return;
        case importButton:emit(SettingsAction::importMetadata);return;
        case checkButton:emit(SettingsAction::checkUpdates);return;
        case installButton:emit(SettingsAction::installUpdate);return;
        case notesButton:
            if(state.canShowReleaseNotes&&!state.updateBusy) emit(SettingsAction::releaseNotes);
            return;
        case mediaButton:emit(SettingsAction::mediaTools);return;
        case repositoryButton:emit(SettingsAction::repository);return;
        default:return;
        }
    }

    LRESULT dispatch(HWND hwnd,UINT message,WPARAM w,LPARAM l)
    {
        switch(message) {
        case WM_SIZE:if(hwnd==window) layout();return 0;
        case WM_CLOSE:hide();return 0;
        case WM_APP+1:layout();return 0;
        case WM_COMMAND:command(LOWORD(w),HIWORD(w));return 0;
        case WM_KEYDOWN:if(w==VK_ESCAPE&&!dropdownOpen()) {hide();return 0;}break;
        case WM_GETMINMAXINFO:
            if(hwnd==window) {
                auto& limits=*reinterpret_cast<MINMAXINFO*>(l);
                limits.ptMinTrackSize={dp(400),dp(360)};return 0;
            }
            break;
        case WM_DPICHANGED:
            if(hwnd==window) {
                dpi=HIWORD(w);makeFonts();
                const RECT next=*reinterpret_cast<RECT*>(l);
                SetWindowPos(window,nullptr,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOZORDER|SWP_NOACTIVATE);
                layout();return 0;
            }
            break;
        case WM_VSCROLL: {
            SCROLLINFO scroll{sizeof(scroll),SIF_TRACKPOS};GetScrollInfo(viewport,SB_VERT,&scroll);
            int next=scrollOffset;
            switch(LOWORD(w)) {
            case SB_TOP:next=0;break;case SB_BOTTOM:next=scrollLimit;break;
            case SB_LINEUP:next-=32;break;case SB_LINEDOWN:next+=32;break;
            case SB_PAGEUP:next-=180;break;case SB_PAGEDOWN:next+=180;break;
            case SB_THUMBPOSITION:case SB_THUMBTRACK:next=scroll.nTrackPos;break;
            default:break;
            }
            scrollTo(next);return 0;
        }
        case WM_MOUSEWHEEL:scrollTo(scrollOffset-MulDiv(GET_WHEEL_DELTA_WPARAM(w),45,WHEEL_DELTA));return 0;
        case WM_ERASEBKGND: {
            RECT bounds{};GetClientRect(hwnd,&bounds);fill(reinterpret_cast<HDC>(w),bounds,background);return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);fill(dc,paint.rcPaint,background);EndPaint(hwnd,&paint);return 0;
        }
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX: {
            HDC dc=reinterpret_cast<HDC>(w);SetTextColor(dc,foreground);
            SetBkColor(dc,message==WM_CTLCOLORSTATIC?background:input);
            return reinterpret_cast<LRESULT>(message==WM_CTLCOLORSTATIC?backgroundBrush:inputBrush);
        }
        case WM_MEASUREITEM: {
            auto& item=*reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if(item.CtlType==ODT_COMBOBOX) {item.itemHeight=dp(27);return TRUE;}
            break;
        }
        case WM_DRAWITEM: {
            auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if(item.CtlType==ODT_COMBOBOX) {
                const bool selected=(item.itemState&ODS_SELECTED)!=0;
                fill(item.hDC,item.rcItem,selected?RGB(54,46,86):input);
                if(item.itemID==static_cast<UINT>(-1)) return TRUE;
                const auto length=SendMessageW(item.hwndItem,CB_GETLBTEXTLEN,item.itemID,0);
                if(length<0||length>2048) return TRUE;
                std::wstring value(static_cast<std::size_t>(length)+1,L'\0');
                SendMessageW(item.hwndItem,CB_GETLBTEXT,item.itemID,reinterpret_cast<LPARAM>(value.data()));
                const auto old=SelectObject(item.hDC,font);SetBkMode(item.hDC,TRANSPARENT);
                SetTextColor(item.hDC,IsWindowEnabled(item.hwndItem)?(selected?accent:foreground):muted);
                RECT bounds=item.rcItem;bounds.left+=dp(7);bounds.right-=dp(7);
                DrawTextW(item.hDC,value.c_str(),static_cast<int>(length),&bounds,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                SelectObject(item.hDC,old);return TRUE;
            }
            const bool active=item.CtlID==static_cast<UINT>(tabGeneral+tab);
            fill(item.hDC,item.rcItem,active?RGB(42,37,69):input);
            const auto old=SelectObject(item.hDC,item.CtlID>=tabGeneral&&item.CtlID<=tabAbout?smallFont:font);
            SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,(item.itemState&ODS_DISABLED)?muted:active?accent:foreground);
            RECT bounds=item.rcItem;bounds.left+=dp(6);bounds.right-=dp(6);
            const auto value=windowText(item.hwndItem);
            DrawTextW(item.hDC,value.c_str(),static_cast<int>(value.size()),&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(item.hDC,old);
            if(item.itemState&ODS_FOCUS) {InflateRect(&bounds,-1,-3);DrawFocusRect(item.hDC,&bounds);}
            return TRUE;
        }
        default:break;
        }
        return DefWindowProcW(hwnd,message,w,l);
    }
};

SettingsWindow::SettingsWindow(HWND owner,Callback callback):impl_(std::make_unique<Impl>(owner,std::move(callback))) {}
SettingsWindow::~SettingsWindow()=default;
void SettingsWindow::show(const SettingsState& state,UINT dpi)
{
    impl_->state=state;
    if(dpi&&dpi!=impl_->dpi) {impl_->dpi=dpi;impl_->makeFonts();}
    impl_->applyState();
    impl_->layout();
    ShowWindow(impl_->window,SW_SHOWNORMAL);
    SetForegroundWindow(impl_->window);
}
void SettingsWindow::update(const SettingsState& state) {impl_->state=state;impl_->applyState();}
void SettingsWindow::hide() {impl_->hide();}
bool SettingsWindow::visible() const {return IsWindowVisible(impl_->window)!=FALSE;}
HWND SettingsWindow::window() const {return impl_->window;}
bool SettingsWindow::processMessage(MSG& message)
{
    if(message.hwnd!=impl_->window&&!IsChild(impl_->window,message.hwnd)) return false;
    if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE&&!impl_->dropdownOpen()) {hide();return true;}
    if(message.message==WM_KEYDOWN&&message.wParam==VK_TAB) return IsDialogMessageW(impl_->window,&message)!=FALSE;
    if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN) {
        wchar_t type[32]{};GetClassNameW(message.hwnd,type,32);
        if(_wcsicmp(type,L"BUTTON")==0) {SendMessageW(message.hwnd,BM_CLICK,0,0);return true;}
    }
    return false;
}

} // namespace viewery
