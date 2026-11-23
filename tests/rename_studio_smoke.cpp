#include "rename_studio.hpp"
#include "rename_template.hpp"
#include "frame_image.hpp"

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <algorithm>
#include <commctrl.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
using namespace viewery;
using namespace std::chrono_literals;
constexpr UINT changed=WM_APP+500;
constexpr int closeStudio=1051;
constexpr int advancedOptions=1052;
constexpr int liveExample=1054;
unsigned closeRequests=0;

HWND control(HWND pane,int id) {
    struct Search {int id;HWND found=nullptr;} search{id};
    EnumChildWindows(pane,[](HWND child,LPARAM parameter)->BOOL {
        auto& search=*reinterpret_cast<Search*>(parameter);
        wchar_t parentClass[80]{};
        GetClassNameW(GetParent(child),parentClass,static_cast<int>(std::size(parentClass)));
        // Native combo edit children also use ID 1001, which is a Studio tab ID.
        const bool internalCombo=_wcsicmp(parentClass,L"ComboBox")==0||_wcsicmp(parentClass,L"ComboLBox")==0;
        if(GetDlgCtrlID(child)==search.id&&!internalCombo) {search.found=child;return FALSE;}
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.found;
}

LRESULT CALLBACK hostProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_COMMAND&&LOWORD(wparam)==closeStudio) {
        ++closeRequests;
        if(const HWND pane=FindWindowExW(window,nullptr,L"Viewery.RenameStudio.1",nullptr)) ShowWindow(pane,SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void require(bool condition,const char* label) {
    std::cout<<(condition?"OK   ":"FAIL ")<<label<<std::endl;
    if(!condition) throw std::runtime_error(label);
}

void pump() {
    MSG msg{};
    while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
}

bool wait(auto predicate,unsigned milliseconds=8000) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    do {
        pump(); if(predicate()) return true; Sleep(10);
    } while(std::chrono::steady_clock::now()<until);
    return false;
}

std::wstring text(HWND control) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control))+1,L'\0');
    const int n=GetWindowTextW(control,value.data(),static_cast<int>(value.size()));
    value.resize(static_cast<std::size_t>(n));return value;
}

std::string content(const fs::path& path) {
    std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});
}

void notify(HWND pane,int id,int notification) {
    const auto child=control(pane,id);
    SendMessageW(GetParent(child),WM_COMMAND,MAKEWPARAM(id,notification),reinterpret_cast<LPARAM>(child));
}
void click(HWND pane,int id) {notify(pane,id,BN_CLICKED);}

RECT bounds(HWND child,HWND relativeTo=nullptr) {
    RECT result{};GetWindowRect(child,&result);
    if(relativeTo) MapWindowPoints(nullptr,relativeTo,reinterpret_cast<POINT*>(&result),2);
    return result;
}
bool equalBounds(RECT first,RECT second) {
    return EqualRect(&first,&second)!=FALSE;
}

struct HiddenCaret {
    HWND owner=GetFocus();
    bool hidden=owner&&HideCaret(owner)!=FALSE;
    ~HiddenCaret() {if(hidden) ShowCaret(owner);}
};

std::vector<std::wstring> blockLabels(HWND list) {
    std::vector<std::wstring> labels;
    for(LRESULT row=0;row<SendMessageW(list,LB_GETCOUNT,0,0);++row) {
        std::wstring value(static_cast<std::size_t>(SendMessageW(list,LB_GETTEXTLEN,row,0))+1,L'\0');
        const auto size=SendMessageW(list,LB_GETTEXT,row,reinterpret_cast<LPARAM>(value.data()));
        value.resize(static_cast<std::size_t>(size));labels.push_back(std::move(value));
    }
    return labels;
}

std::jthread confirmOwnDialog(DWORD ownerThread) {
    return std::jthread([ownerThread](std::stop_token stop) {
        while(!stop.stop_requested()) {
            struct Search { HWND dialog=nullptr; } search;
            EnumThreadWindows(ownerThread,[](HWND hwnd,LPARAM data)->BOOL {
                wchar_t klass[40]{};GetClassNameW(hwnd,klass,40);
                if(std::wstring_view(klass)==L"#32770"&&GetDlgItem(hwnd,IDYES)&&IsWindowVisible(hwnd)) {
                    reinterpret_cast<Search*>(data)->dialog=hwnd;return FALSE;
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(&search));
            if(search.dialog) {
                PostMessageW(search.dialog,WM_COMMAND,MAKEWPARAM(IDYES,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(search.dialog,IDYES)));
                return;
            }
            Sleep(10);
        }
    });
}

bool capture(HWND host,CapturedFrame& frame,bool backing=false) {
    RECT rect{};GetClientRect(host,&rect);
    frame.width=rect.right;frame.height=rect.bottom;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=rect.right;
    info.bmiHeader.biHeight=-rect.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap||!pixels) {if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return false;}
    const auto old=SelectObject(dc,bitmap);
    HDC source=backing?GetDC(host):nullptr;
    const bool rendered=backing?BitBlt(dc,0,0,rect.right,rect.bottom,source,0,0,SRCCOPY)!=FALSE:PrintWindow(host,dc,2)!=FALSE;
    if(source) ReleaseDC(host,source);
    if(rendered) {
        const auto* begin=static_cast<std::uint8_t*>(pixels);
        frame.bgra.assign(begin,begin+static_cast<std::size_t>(rect.right)*rect.bottom*4);
        for(std::size_t i=3;i<frame.bgra.size();i+=4) frame.bgra[i]=255;
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    return rendered;
}

bool screenshot(HWND host,const fs::path& output) {
    CapturedFrame frame;
    return capture(host,frame)&&saveFramePng(output,frame);
}

bool containsPanelBackground(const CapturedFrame& frame) {
    for(std::size_t offset=0;offset+3<frame.bgra.size();offset+=4) {
        if(frame.bgra[offset]==33&&frame.bgra[offset+1]==26&&frame.bgra[offset+2]==23) return true;
    }
    return false;
}

void sizePanel(RenameStudio& studio,HWND host,UINT dpi,int height,HFONT font,int width=510) {
    RECT area{0,0,MulDiv(width,static_cast<int>(dpi),96),MulDiv(height,static_cast<int>(dpi),96)};
    SetWindowPos(host,nullptr,0,0,area.right+40,area.bottom+80,SWP_NOZORDER|SWP_NOACTIVATE);
    studio.show(area,dpi,font);pump();
}

void checkScrollingLayout(RenameStudio& studio,HWND host,const fs::path& folder) {
    const HWND pane=studio.window();
    for(const UINT dpi:{96U,120U,144U}) {
        HFONT scaledFont=CreateFontW(-MulDiv(14,static_cast<int>(dpi),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        for(const int width:{380,510}) {
        for(const int height:{340,460,700}) {
            sizePanel(studio,host,dpi,height,scaledFont,width);
            for(const int tab:{1000,1001,1002,1003}) {
                click(pane,tab);pump();
                require(control(pane,closeStudio)!=nullptr,"Studio provides a close button on every tab");
                const auto firstTab=bounds(control(pane,1000),pane);
                const auto close=bounds(control(pane,closeStudio),pane);
                const auto footer=bounds(control(pane,1005),pane);
                const HWND body=control(pane,1055);
                require(body!=pane&&IsChild(pane,body),"Form controls are inside a clipped scroll viewport");
                const auto viewport=bounds(body,pane);
                SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);
                SetFocus(pane);
                HiddenCaret caret;
                RedrawWindow(pane,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
                CapturedFrame originalPixels;
                bool backing=capture(pane,originalPixels,true)&&containsPanelBackground(originalPixels);
                if(!backing) require(capture(pane,originalPixels),"Native panel renderer produces a nonempty baseline");
                bool stable=viewport.top>=firstTab.bottom&&viewport.bottom<=footer.top;
                for(int iteration=0;iteration<8;++iteration) {
                    SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_BOTTOM,0),0);
                    SendMessageW(body,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);
                    pump();
                    stable=stable&&equalBounds(firstTab,bounds(control(pane,1000),pane))&&
                        equalBounds(close,bounds(control(pane,closeStudio),pane))&&
                        equalBounds(footer,bounds(control(pane,1005),pane))&&equalBounds(viewport,bounds(body,pane));
                    if(iteration==0&&(tab==1000||tab==1001)) {
                        CapturedFrame scrolled;
                        const auto name=L"scrolled-"+std::to_wstring(dpi)+L"-"+std::to_wstring(width)+L"-"+
                            std::to_wstring(height)+L"-"+std::to_wstring(tab)+L".png";
                        require(capture(pane,scrolled,backing)&&saveFramePng(folder/name,scrolled),
                            "Actual scrolled backing pixels are saved for visual inspection");
                    }
                    SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);
                    SendMessageW(body,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);pump();
                }
                require(stable,"Scrolling keeps tabs, close button and footer fixed without form overlap");
                SetFocus(pane);
                CapturedFrame finalPixels;
                const bool captured=capture(pane,finalPixels,backing);
                const bool identical=captured&&originalPixels.bgra==finalPixels.bgra;
                if(!identical) {
                    const auto suffix=std::to_wstring(dpi)+L"-"+std::to_wstring(width)+L"-"+std::to_wstring(height)+L"-"+std::to_wstring(tab)+L".png";
                    saveFramePng(folder/(L"pixel-original-"+suffix),originalPixels);
                    if(captured) saveFramePng(folder/(L"pixel-after-"+suffix),finalPixels);
                }
                require(identical,
                    "Scroll round trips preserve identical native panel pixels without ghost labels or buttons");
                std::cout<<"CAPTURE dpi="<<dpi<<" width="<<width<<" height="<<height<<" tab="<<tab<<" mode="<<(backing?"window-dc":"print-window")<<'\n';
                RECT client{};GetClientRect(pane,&client);
                bool fits=true;
                for(const int id:{1000,1001,1002,1003,closeStudio}) {
                    const auto box=bounds(control(pane,id),pane);
                    fits=fits&&box.left>=0&&box.top>=0&&box.right>box.left&&box.bottom>box.top&&
                        box.right<=client.right&&box.bottom<=client.bottom;
                }
                require(fits,"Navigation and close controls fit at 100, 125 and 150 percent DPI");
                bool readable=true;
                for(int id=1000;id<=1054;++id) {
                    const HWND child=control(pane,id);
                    if(!child||!IsWindowVisible(child)) continue;
                    const auto box=bounds(child,pane);
                    RECT intersection{};
                    if(GetParent(child)!=pane&&!IntersectRect(&intersection,&box,&viewport)) continue;
                    if(GetParent(child)!=pane&&(box.left<viewport.left||box.right>viewport.right)) readable=false;
                    wchar_t type[40]{};GetClassNameW(child,type,static_cast<int>(std::size(type)));
                    if(_wcsicmp(type,L"Button")==0) {
                        HDC dc=GetDC(child);
                        const auto previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
                        const auto value=text(child);SIZE extent{};
                        GetTextExtentPoint32W(dc,value.data(),static_cast<int>(value.size()),&extent);
                        readable=readable&&extent.cx+MulDiv(10,static_cast<int>(dpi),96)<=box.right-box.left;
                        SelectObject(dc,previous);ReleaseDC(child,dc);
                    }
                }
                require(readable,"Visible form controls fit horizontally and button captions fit their available width");
                if(tab==1000||tab==1001) {
                    const auto name=L"layout-"+std::to_wstring(dpi)+L"-"+std::to_wstring(width)+L"-"+std::to_wstring(height)+L"-"+std::to_wstring(tab)+L".png";
                    require(screenshot(pane,folder/name),"Scrolled panel screenshot is saved from its private host");
                }
            }
        }
        }
        // A font must not be deleted while any native child still uses it.
        HFONT stock=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        sizePanel(studio,host,96,700,stock);
        DeleteObject(scaledFont);
    }
}

void checkBlockMouseInteraction(HWND pane) {
    click(pane,1001);pump();
    const HWND list=control(pane,1021);
    const HWND body=control(pane,1055);
    SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);
    SendMessageW(body,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);pump();
    const auto initial=blockLabels(list);
    require(initial.size()>=4,"Template builder contains independently selectable blocks");
    RECT third{};SendMessageW(list,LB_GETITEMRECT,2,reinterpret_cast<LPARAM>(&third));
    const POINT clickPoint{third.left+12,(third.top+third.bottom)/2};
    SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(clickPoint.x,clickPoint.y));
    SendMessageW(list,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(clickPoint.x+1,clickPoint.y+1));
    SendMessageW(list,WM_LBUTTONUP,0,MAKELPARAM(clickPoint.x+1,clickPoint.y+1));pump();
    require(SendMessageW(list,LB_GETCURSEL,0,0)==2&&blockLabels(list)==initial,
        "A normal block click with tiny pointer motion selects without reordering");
    RECT first{};SendMessageW(list,LB_GETITEMRECT,0,reinterpret_cast<LPARAM>(&first));
    const POINT start{first.left+12,(first.top+first.bottom)/2};
    const POINT finish{third.left+12,(third.top+third.bottom)/2};
    SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(start.x,start.y));
    SendMessageW(list,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(finish.x,finish.y));
    SendMessageW(list,WM_LBUTTONUP,0,MAKELPARAM(finish.x,finish.y));pump();
    auto expected=initial;const auto moved=expected.front();expected.erase(expected.begin());expected.insert(expected.begin()+2,moved);
    require(blockLabels(list)==expected,"Dragging beyond the threshold reorders a block while the form is scrolled");
    SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(finish.x,finish.y));
    SendMessageW(list,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(start.x,start.y));
    SendMessageW(list,WM_LBUTTONUP,0,MAKELPARAM(start.x,start.y));pump();
    require(blockLabels(list)==initial,"Dragging a block back restores the original template order");
}

void checkPresetDropdowns(RenameStudio& studio,HWND host,const fs::path& folder) {
    const HWND pane=studio.window();
    for(const UINT dpi:{96U,120U,144U}) {
        HFONT font=CreateFontW(-MulDiv(14,static_cast<int>(dpi),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        sizePanel(studio,host,dpi,460,font);click(pane,1001);pump();
        const HWND choice=control(pane,1015);
        SendMessageW(control(pane,1055),WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);
        SendMessageW(choice,CB_SHOWDROPDOWN,TRUE,0);pump();
        COMBOBOXINFO info{sizeof(info)};
        require(GetComboBoxInfo(choice,&info)&&IsWindowVisible(info.hwndList)&&SendMessageW(choice,CB_GETDROPPEDSTATE,0,0),
                "Preset chooser opens its real native popup at each supported DPI");
        const auto suffix=std::to_wstring(dpi)+L".png";
        require(screenshot(pane,folder/(L"preset-panel-open-"+suffix))&&screenshot(info.hwndList,folder/(L"preset-popup-"+suffix)),
                "Expanded preset chooser and native popup screenshots are saved");
        const auto row=SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(L"Smoke preset"));
        SendMessageW(choice,CB_SETCURSEL,row,0);notify(pane,1015,CBN_SELCHANGE);
        SendMessageW(choice,CB_SHOWDROPDOWN,FALSE,0);pump();
        require(text(choice)==L"Smoke preset"&&screenshot(pane,folder/(L"preset-selected-"+suffix)),
                "Selected preset remains readable and renders after the popup is closed");
        sizePanel(studio,host,96,700,static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
        DeleteObject(font);
    }
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    const fs::path outputRoot=fs::absolute(argv[1]);
    const fs::path folder=outputRoot/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto desktopName=L"VieweryStudioQA-"+std::to_wstring(GetCurrentProcessId());
    const HDESK previousDesktop=GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop=CreateDesktopW(desktopName.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop||!SetThreadDesktop(desktop)) {
        const DWORD error=GetLastError();
        if(desktop)CloseDesktop(desktop);
        std::cout<<"SKIP Private desktop unavailable (Win32 "<<error<<"). No UI or collection was opened.\n";
        return 77;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)) {SetThreadDesktop(previousDesktop);CloseDesktop(desktop);return 4;}
    HWND host=nullptr;
    HFONT font=nullptr;
    try {
        fs::create_directories(folder);
        const INITCOMMONCONTROLSEX init{sizeof(init),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};InitCommonControlsEx(&init);
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=hostProcedure;
        wc.lpszClassName=L"Viewery.StudioQAHost.1";RegisterClassW(&wc);
        host=CreateWindowExW(0,wc.lpszClassName,L"Viewery Rename Studio",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,650,850,nullptr,nullptr,wc.hInstance,nullptr);
        require(host!=nullptr,"Private desktop host window is created");
        font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        const auto one=folder/L"image001.png",two=folder/L"image002.png",three=folder/L"keep.png";
        {std::ofstream f(one,std::ios::binary);f<<"first bytes";}
        {std::ofstream f(two,std::ios::binary);f<<"second bytes";}
        {std::ofstream f(three,std::ios::binary);f<<"unselected bytes";}
        unsigned beforeChanges=0;
        auto studio=std::make_unique<RenameStudio>(host,changed,[&]{++beforeChanges;},folder/L"collection.sqlite3");
        RECT client{};GetClientRect(host,&client);studio->show(client,96,font);
        studio->setFiles({one,two,three},{one,two});
        require(wait([&]{return !studio->busy()&&studio->snapshot().files.size()==3;}),"Studio indexes only its explicit isolated test database");
        const HWND pane=studio->window();
        SetWindowTextW(control(pane,1006),L"Artist A");
        SetWindowTextW(control(pane,1007),L"First Light");
        SendMessageW(control(pane,1009),CB_SETCURSEL,1,0);
        SetWindowTextW(control(pane,1013),L"tag=alpha\r\ntag=beta");
        click(pane,1012);
        require(!studio->busy()&&studio->snapshot().artists.empty()&&text(control(pane,1005)).find(L"more than once")!=std::wstring::npos,
                "Duplicate custom field names are rejected before mutation");
        SetWindowTextW(control(pane,1013),L"tag=alpha\r\nsource=artist=portal");
        click(pane,1012);
        require(wait([&]{return !studio->busy()&&studio->snapshot().artworks.size()==1&&studio->snapshot().versions.size()==2;}),
                "Metadata assignment groups selected files into versions of one work");
        require(std::count_if(studio->snapshot().files.begin(),studio->snapshot().files.end(),[](const MediaRecord& file){
            return !file.artworkId.empty()&&file.fields.contains(L"source")&&file.fields.at(L"source")==L"artist=portal";})==2,
                "Multiline fields preserve values including equals signs and affect selected files only");
        const auto originalWork=studio->snapshot().artworks.front().id;
        const auto originalVersion=studio->snapshot().files.front().versionId;
        SetWindowTextW(control(pane,1006),L"Artist B");SetWindowTextW(control(pane,1007),L"");
        SendMessageW(control(pane,1009),CB_SETCURSEL,3,0);click(pane,1012);
        require(wait([&]{return !studio->busy();})&&studio->snapshot().artworks.size()==1&&studio->snapshot().versions.size()==2&&
                studio->snapshot().artworks.front().id==originalWork&&studio->snapshot().files.front().versionId==originalVersion,
                "Artist-only assignment preserves work and version identities and numbering");
        SetWindowTextW(control(pane,1006),L"Artist A");click(pane,1012);
        require(wait([&]{return !studio->busy();}),"Artist-only assignment can restore the original artist");
        click(pane,1001);
        SetWindowTextW(control(pane,1034),L" + ");SetWindowTextW(control(pane,1035),L"8");
        SendMessageW(control(pane,1036),CB_SETCURSEL,1,0);
        SendMessageW(control(pane,1021),LB_SETCURSEL,0,0);
        notify(pane,1021,LBN_SELCHANGE);
        SetWindowTextW(control(pane,1029),L"A-");click(pane,1033);
        require(text(control(pane,1034))==L" + "&&text(control(pane,1035))==L"8"&&
                SendMessageW(control(pane,1036),CB_GETCURSEL,0,0)==1,"Editing a block preserves unsaved global template settings");
        SetWindowTextW(control(pane,1015),L"Smoke preset");click(pane,1016);
        require(wait([&]{return !studio->busy();}),"Visual template saves successfully");
        {
            CollectionStore verify(folder/L"collection.sqlite3");const auto presets=verify.presets();
            require(presets.size()==1,"Saved template is persisted once");
            const auto bytes=presets.front().json;
            const int size=WideCharToMultiByte(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0,nullptr,nullptr);
            std::string json(static_cast<std::size_t>(size),'\0');WideCharToMultiByte(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),json.data(),size,nullptr,nullptr);
            const auto preset=parseTemplate(json);
            require(preset.blocks.front().prefix==L"A-"&&preset.counterScope==CounterScope::artist&&preset.counterStart==8&&preset.authorSeparator==L" + ",
                    "Persisted template retains block and global configuration");
        }
        click(pane,1001);
        if(!IsWindowVisible(control(pane,1029))) click(pane,advancedOptions);
        require(IsWindowVisible(control(pane,1029)),"Advanced block fields can be opened explicitly");
        SetWindowTextW(control(pane,1029),L"B-");click(pane,1033);
        SetWindowTextW(control(pane,1015),L"Other preset");click(pane,1017);
        require(wait([&]{return !studio->busy();})&&SendMessageW(control(pane,1015),CB_GETCOUNT,0,0)==2,
                "A second preset can be created and remains available in the chooser");
        {
            const HWND choice=control(pane,1015),body=control(pane,1055);
            SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);
            SendMessageW(choice,CB_SHOWDROPDOWN,TRUE,0);pump();
            const auto before=bounds(choice,pane);
            SendMessageW(body,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);pump();
            require(SendMessageW(choice,CB_GETDROPPEDSTATE,0,0)&&equalBounds(before,bounds(choice,pane)),
                    "An open preset dropdown stays in place when the form receives a wheel event");
            const auto original=SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(L"Smoke preset"));
            require(original!=CB_ERR,"Saved presets are individually selectable");
            SendMessageW(choice,CB_SETCURSEL,original,0);notify(pane,1015,CBN_SELCHANGE);
            SendMessageW(choice,CB_SHOWDROPDOWN,FALSE,0);pump();
            SendMessageW(control(pane,1021),LB_SETCURSEL,0,0);notify(pane,1021,LBN_SELCHANGE);
            require(text(control(pane,1029))==L"A-"&&text(control(pane,1015))==L"Smoke preset",
                    "Choosing a saved preset restores its block settings without corrupting the selection");
            require(text(control(pane,liveExample)).find(L"First Light")!=std::wstring::npos,
                    "The constructor displays a live filename example from the selected artwork");
        }
        checkBlockMouseInteraction(pane);
        checkPresetDropdowns(*studio,host,folder);
        checkScrollingLayout(*studio,host,folder);
        RECT originalClient{};GetClientRect(host,&originalClient);studio->show(originalClient,96,font);
        click(pane,1001);
        if(IsWindowVisible(control(pane,1029))) click(pane,advancedOptions);
        SendMessageW(control(pane,1055),WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);pump();
        require(screenshot(pane,folder/L"rename-template.png"),"Native visual template screenshot is saved");
        click(pane,1002);click(pane,1041);
        studio->setFiles({one,two,three},{three});
        require(wait([&]{return !studio->busy();})&&ListView_GetItemCount(control(pane,1037))==0,
                "Changing selection discards an old asynchronous rename preview");
        studio->setFiles({one,two,three},{one,two});click(pane,1041);
        require(wait([&]{return !studio->busy()&&ListView_GetItemCount(control(pane,1037))==2;}),
                "New selection produces a two-file rename preview");
        ListView_SetItemState(control(pane,1037),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);pump();
        require(screenshot(pane,folder/L"rename-studio-preview.png"),"Native Rename Studio screenshot is saved from the isolated host");
        {
            auto confirm=confirmOwnDialog(GetCurrentThreadId());click(pane,1042);
        }
        const auto renamedOne=folder/L"A-Artist A_001_v01_First Light.png";
        const auto renamedTwo=folder/L"A-Artist A_001_v02_First Light.png";
        require(wait([&]{return !studio->busy()&&fs::exists(renamedOne)&&fs::exists(renamedTwo);}),
                "Confirmed rename changes the selected two files");
        require(beforeChanges==1&&content(renamedOne)=="first bytes"&&content(renamedTwo)=="second bytes"&&content(three)=="unselected bytes",
                "Rename preserves original content and unselected file");
        studio->takeFileChanges();studio->setFiles({renamedOne,renamedTwo,three},{renamedOne,renamedTwo});
        require(wait([&]{return !studio->busy();}),"Studio refreshes renamed file identities");
        click(pane,1003);
        require(ListView_GetItemCount(control(pane,1043))==1,"History lists the completed rename");
        ListView_SetItemState(control(pane,1043),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        require(wait([&]{return !studio->busy();})&&text(control(pane,1050)).find(L"image001.png")!=std::wstring::npos,"History exposes old and new paths");
        {
            auto confirm=confirmOwnDialog(GetCurrentThreadId());click(pane,1044);
        }
        require(wait([&]{return !studio->busy()&&fs::exists(one)&&fs::exists(two);}),"Undo restores original filenames through the native history panel");
        require(content(one)=="first bytes"&&content(two)=="second bytes"&&beforeChanges==2,
                "Undo preserves both file contents");
        const auto artistCount=studio->snapshot().artists.size();
        const auto closeBefore=closeRequests;
        click(pane,closeStudio);
        require(wait([&]{return closeRequests==closeBefore+1;})&&!IsWindowVisible(pane),
                "The close button hides Studio without disabling or destroying the module");
        studio->show(originalClient,96,font);click(pane,1000);pump();
        require(IsWindowVisible(pane)&&studio->snapshot().artists.size()==artistCount&&studio->snapshot().files.size()==3,
                "Reopening the closed panel preserves its collection and selection");
        COMBOBOXINFO comboInfo{sizeof(comboInfo)};
        require(GetComboBoxInfo(control(pane,1006),&comboInfo)&&comboInfo.hwndItem,
                "Editable artist chooser exposes its native text input");
        SetFocus(comboInfo.hwndItem);
        SendMessageW(comboInfo.hwndItem,WM_KEYDOWN,VK_ESCAPE,0);
        require(wait([&]{return closeRequests==closeBefore+2;})&&!IsWindowVisible(pane),
                "Escape from a nested editable field closes Studio without disabling the module");
        studio->show(originalClient,96,font);
        std::cout<<"Studio GUI checks passed.\n";
        const auto artifactPath=folder.u8string();
        std::cout<<"Artifacts: ";
        std::cout.write(reinterpret_cast<const char*>(artifactPath.data()),static_cast<std::streamsize>(artifactPath.size()));
        std::cout<<'\n';
        studio.reset();
        DeleteObject(font);font=nullptr;
        DestroyWindow(host);host=nullptr;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<std::endl;
        if(host)DestroyWindow(host);if(font)DeleteObject(font);CoUninitialize();SetThreadDesktop(previousDesktop);CloseDesktop(desktop);return 1;
    }
    CoUninitialize();SetThreadDesktop(previousDesktop);CloseDesktop(desktop);return 0;
}




