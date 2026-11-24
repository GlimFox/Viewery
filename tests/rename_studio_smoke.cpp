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
unsigned collectionNotifications=0;
unsigned artistRegistryRequests=0;

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
    if(message==WM_COMMAND&&LOWORD(wparam)==RenameStudio::artistsCommandId) {++artistRegistryRequests;return 0;}
    if(message==changed) {++collectionNotifications;return 0;}
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
            for(const int tab:{1000,1002,1003}) {
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
                if(tab==1000) {
                    const auto artistInput=bounds(control(pane,1006),pane);
                    const auto registryButton=bounds(control(pane,RenameStudio::artistsCommandId),pane);
                    require(IsWindowVisible(control(pane,RenameStudio::artistsCommandId))&&registryButton.left>artistInput.right&&
                        registryButton.top==artistInput.top&&registryButton.right<=viewport.right&&
                        registryButton.right-registryButton.left==MulDiv(30,static_cast<int>(dpi),96)&&
                        registryButton.bottom-registryButton.top==MulDiv(30,static_cast<int>(dpi),96),
                        "Compact artist registry button fits beside the editable artist field at every tested DPI");
                }
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

std::wstring cell(HWND list,int row,int column) {
    wchar_t value[1024]{};ListView_GetItemText(list,row,column,value,static_cast<int>(std::size(value)));return value;
}
std::vector<std::pair<std::wstring,std::wstring>> queueNames(HWND pane) {
    const HWND list=control(pane,1037);std::vector<std::pair<std::wstring,std::wstring>> rows;
    for(int row=0;row<ListView_GetItemCount(list);++row) rows.emplace_back(cell(list,row,0),cell(list,row,1));
    return rows;
}
void chooseRow(HWND pane,int row) {
    const HWND list=control(pane,1037);
    ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);pump();
}
HWND templatePane(RenameStudio& studio) {
    studio.openTemplates();pump();
    return FindWindowExW(studio.templatesWindow(),nullptr,L"Viewery.RenameStudio.1",nullptr);
}
void checkOwnedEditor(RenameStudio& studio,const fs::path& folder) {
    const HWND pane=templatePane(studio),host=studio.templatesWindow();
    require(pane&&IsWindowVisible(host)&&GetWindow(host,GW_OWNER)!=nullptr,"Templates open in a separate owned modeless window");
    require((GetWindowLongPtrW(host,GWL_STYLE)&WS_THICKFRAME)!=0,"The template editor is resizable");
    for(const int width:{460,700}) for(const int height:{460,740}) {
        SetWindowPos(host,nullptr,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);pump();
        const HWND body=control(pane,1055);
        SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);
        SetFocus(pane);HiddenCaret caret;
        RedrawWindow(pane,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        CapturedFrame before,after;bool backing=capture(pane,before,true)&&containsPanelBackground(before);
        if(!backing) require(capture(pane,before),"Template editor produces a native baseline");
        const auto close=bounds(control(pane,closeStudio),pane);
        for(int step=0;step<8;++step) {
            SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_BOTTOM,0),0);pump();
            SendMessageW(body,WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);pump();
        }
        require(equalBounds(close,bounds(control(pane,closeStudio),pane))&&capture(pane,after,backing)&&before.bgra==after.bgra,
            "Resized separate editor retains stable header and identical pixels after scrolling");
    }
    const HWND choice=control(pane,1015);
    SendMessageW(choice,CB_SHOWDROPDOWN,TRUE,0);pump();
    COMBOBOXINFO info{sizeof(info)};
    require(GetComboBoxInfo(choice,&info)&&IsWindowVisible(info.hwndList),"Separate editor opens its native preset chooser");
    require(screenshot(info.hwndList,folder/L"template-preset-popup.png"),"Preset chooser screenshot is saved");
    const auto row=SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(L"Batch preset"));
    SendMessageW(choice,CB_SETCURSEL,row,0);notify(pane,1015,CBN_SELCHANGE);
    SendMessageW(choice,CB_SHOWDROPDOWN,FALSE,0);pump();
    require(text(choice)==L"Batch preset","Saved templates can be selected in their separate editor");
    checkBlockMouseInteraction(pane);
    SendMessageW(control(pane,1055),WM_VSCROLL,MAKEWPARAM(SB_TOP,0),0);pump();
    require(screenshot(pane,folder/L"rename-template.png"),"Separate template editor screenshot is saved");
    SendMessageW(host,WM_CLOSE,0,0);pump();
    require(!IsWindowVisible(host)&&IsWindowVisible(studio.window()),"Closing templates leaves the main rename panel open");
}

void checkArtistRegistryRefresh(HWND host,HFONT font,const fs::path& folder) {
    const auto database=folder/L"registry-refresh.sqlite3";
    CollectionStore registry(database);
    Artist artist;artist.name=L"Художник Один";artist.fileName=L"artist-tag";artist.aliases=L"Fox; Лис";
    artist.id=registry.saveArtist(artist);
    Artist removed;removed.name=L"To Delete";removed.fileName=L"delete-tag";removed.aliases=L"Gone";
    removed.id=registry.saveArtist(removed);
    RenameStudio studio(host,changed,[]{},database);
    RECT area{};GetClientRect(host,&area);studio.show(area,96,font);
    studio.refreshCollection();
    require(studio.busy()&&wait([&]{return !studio.busy();}),"Registry-only refresh runs asynchronously before any files are indexed");
    const auto pane=studio.window(),choice=control(pane,1006);
    const auto row=SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(artist.name.c_str()));
    require(studio.snapshot().files.empty()&&SendMessageW(choice,CB_GETCOUNT,0,0)==2&&row>=0,
        "All predefined artists are available even when they have no files");
    SendMessageW(choice,CB_SETCURSEL,row,0);notify(pane,1006,CBN_SELCHANGE);
    require(text(choice)==artist.name,"A predefined artist can be selected from the native data combo");
    const auto oldRemoved=removed;removed.name=L"Renamed Empty Artist";
    const auto metadataRename=registry.renameArtist(oldRemoved,removed,{});
    require(metadataRename.success&&metadataRename.changes.empty(),"An artist with no media gets a durable metadata-only rename operation");
    studio.refreshCollection();require(wait([&]{return !studio.busy();}),"Registry refresh reloads a metadata-only artist operation");
    click(pane,1003);
    require(ListView_GetItemCount(control(pane,1043))==1&&cell(control(pane,1043),0,2)==L"0"&&registry.history().front().canUndo,
        "Studio history exposes Undo for an artist rename without requiring another file index");
    click(pane,1000);

    const auto source=folder/L"registry-original.png";
    {std::ofstream output(source,std::ios::binary);output<<"registry bytes";}
    studio.setFiles({source},{source});require(wait([&]{return !studio.busy();}),"Registry test source is indexed in the isolated database");
    const auto originalBytes=studio.snapshot().files.front().bytes;
    const auto editorPane=templatePane(studio);
    SendMessageW(control(editorPane,1021),LB_SETCURSEL,0,0);notify(editorPane,1021,LBN_SELCHANGE);
    SetWindowTextW(control(editorPane,1029),L"R-");click(editorPane,1033);click(editorPane,1041);pump();
    SetWindowTextW(choice,L"aRtIsT-TaG; fOX; лИС");notify(pane,1006,CBN_EDITCHANGE);
    const auto draft=text(control(pane,liveExample));click(pane,1041);
    require(wait([&]{return !studio.busy()&&studio.queuedFileCount()==1;}),"Filename tokens and Unicode aliases can prepare a batch using an existing artist");
    const auto frozen=queueNames(pane);
    require(studio.snapshot().artists.size()==2&&studio.snapshot().artworks.front().artists==std::vector<LibraryId>{artist.id}&&
        frozen.front().second==draft&&frozen.front().second.find(L"R-artist-tag_")==0,
        "Case-insensitive aliases share one artist ID and the live example keeps canonical filename spelling");

    SetWindowTextW(choice,L"Fox; Gone; Unrelated Draft");
    artist.name=L"Переименованный Художник";artist.fileName=L"new-tag";artist.aliases=L"New Alias";
    registry.saveArtist(artist);registry.deleteArtist(removed.id);
    {std::ofstream output(source,std::ios::binary|std::ios::trunc);output<<"physical file changed without a reindex request";}
    const auto notifications=collectionNotifications;
    studio.refreshCollection();studio.refreshCollection();studio.refreshCollection();
    require(wait([&]{return !studio.busy();}),"Repeated refresh requests coalesce while an asynchronous registry refresh runs");pump();
    require(collectionNotifications==notifications+2,"A busy refresh keeps only one pending reload");
    require(text(choice)==artist.name+L"; Unrelated Draft"&&SendMessageW(choice,CB_GETCOUNT,0,0)==1&&
        SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(artist.name.c_str()))>=0,
        "Registry rename follows the same ID, deletion removes its old alias, and an unrelated typed draft survives");
    require(studio.snapshot().files.front().bytes==originalBytes&&queueNames(pane)==frozen,
        "Registry refresh does not reindex media or alter approved queue names");
    require(text(control(editorPane,1029))==L"R-","Registry refresh preserves the currently edited template");

    SetWindowTextW(choice,L"New Alias");artist.aliases=L"Replacement Alias";registry.saveArtist(artist);
    studio.refreshCollection();require(wait([&]{return !studio.busy();})&&text(choice)==artist.name,
        "Removing an alias keeps its typed token attached to the existing artist ID");

    SetWindowTextW(choice,L"replacement alias; ПЕРЕИМЕНОВАННЫЙ ХУДОЖНИК");click(pane,1041);
    require(studio.busy(),"A new batch starts after registry reconciliation");
    studio.refreshCollection();studio.refreshCollection();
    require(wait([&]{return !studio.busy()&&studio.queuedFileCount()==1;}),"A refresh requested during batch preparation runs after the job without losing its queue");
    require(studio.snapshot().artists.size()==1&&queueNames(pane).front().second.find(L"R-new-tag_")==0,
        "The renamed registry artist is reused without recreating a deleted or duplicate artist");
    registry.deleteArtist(artist.id);studio.refreshCollection();
    require(wait([&]{return !studio.busy();})&&text(choice).empty()&&SendMessageW(choice,CB_GETCOUNT,0,0)==0,
        "Deleting the remaining artist clears every bound semicolon token");
    const auto approved=queueNames(pane);click(pane,1041);
    require(wait([&]{return !studio.busy();})&&studio.snapshot().artists.empty()&&approved.size()==1,
        "Adding another batch after deletion does not silently recreate the deleted artist");
    studio.hide();
}

void checkQueuedCycleUndo(HWND host,HFONT font,const fs::path& folder) {
    const auto first=folder/L"cycle-first.png",second=folder/L"cycle-second.png";
    {std::ofstream output(first,std::ios::binary);output<<"first cycle bytes";}
    {std::ofstream output(second,std::ios::binary);output<<"second cycle bytes";}
    const auto database=folder/L"cycle-queue.sqlite3";
    CollectionStore registry(database);
    const auto files=registry.indexFiles({first,second});
    const auto renamed=registry.rename({{files[0].id,first,second.filename().wstring(),true},
        {files[1].id,second,first.filename().wstring(),true}});
    require(renamed.success,"An isolated two-file rename cycle is committed before preparing another batch");
    RenameStudio studio(host,changed,[]{},database);
    RECT area{};GetClientRect(host,&area);studio.show(area,96,font);studio.setFiles({first,second},{first,second});
    require(wait([&]{return !studio.busy();}),"Studio loads both post-cycle identities");
    const auto pane=studio.window();SetWindowTextW(control(pane,1006),L"Cycle Artist");click(pane,1041);
    require(wait([&]{return !studio.busy()&&studio.queuedFileCount()==2;}),"Both post-cycle files can be frozen in a pending batch");
    const auto before=queueNames(pane);click(pane,1003);
    ListView_SetItemState(control(pane,1043),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);pump();
    require(wait([&]{return !studio.busy();}),"Cycle operation history can be inspected while a batch stays queued");
    {auto confirm=confirmOwnDialog(GetCurrentThreadId());click(pane,1044);}
    require(wait([&]{return !studio.busy();}),"Undo restores the two original file identities");
    const auto after=queueNames(pane);
    require(after.size()==before.size()&&std::ranges::all_of(after,[&](const auto& entry) {
        const auto old=std::ranges::find(before,entry.second,&std::pair<std::wstring,std::wstring>::second);
        return old!=before.end()&&entry.first!=old->first&&
            (entry.first==first.filename().wstring()||entry.first==second.filename().wstring());
    }),"Queued cycle sources remap once while each approved target name stays frozen");
    require(content(first)=="first cycle bytes"&&content(second)=="second cycle bytes",
        "Cycle Undo keeps every file's original bytes with its remapped queued source");
    studio.hide();
}

struct NativeArtistChoice {
    HWND combo=nullptr;
    unsigned changes=0;
    std::wstring textAtChange;
};
LRESULT CALLBACK observeNativeArtistChoice(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR value) {
    auto& observed=*reinterpret_cast<NativeArtistChoice*>(value);
    if(message==WM_COMMAND&&reinterpret_cast<HWND>(lparam)==observed.combo&&HIWORD(wparam)==CBN_SELCHANGE) {
        ++observed.changes;observed.textAtChange=text(observed.combo);
    }
    return DefSubclassProc(window,message,wparam,lparam);
}
void checkNativeArtistDropdown(HWND host,HFONT font,const fs::path& folder) {
    const auto database=folder/L"native-artist-choice.sqlite3",source=folder/L"glimfox_001_v01_aaa.png";
    {std::ofstream output(source,std::ios::binary);output<<"native dropdown source";}
    CollectionStore registry(database);
    Artist alpha;alpha.name=L"Alpha Display";alpha.fileName=L"Худ1";alpha.id=registry.saveArtist(alpha);
    Artist fox;fox.name=L"glimfox";fox.fileName=L"fox-token";fox.id=registry.saveArtist(fox);
    auto file=registry.indexFiles({source}).front();
    Artwork old;old.artists={alpha.id};old.number=6;old.title=L"Stored Old Title";old.userTitle=L"Stored Old Override";
    old.id=registry.saveArtwork(old);file.artworkId=old.id;registry.saveFile(file);
    RenameStudio studio(host,changed,[]{},database);
    RECT area{};GetClientRect(host,&area);studio.show(area,96,font);studio.setFiles({source},{source});
    require(wait([&]{return !studio.busy();}),"Native artist selection fixture loads distinct display names and filename tokens");
    const auto pane=studio.window(),choice=control(pane,1006);
    const auto editorPane=templatePane(studio);
    SendMessageW(control(editorPane,1021),LB_SETCURSEL,0,0);notify(editorPane,1021,LBN_SELCHANGE);
    SetWindowTextW(control(editorPane,1029),L"Custom-");click(editorPane,1033);click(editorPane,1041);pump();
    const auto alphaRow=SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(alpha.name.c_str()));
    const auto foxRow=SendMessageW(choice,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(fox.name.c_str()));
    require(alphaRow>=0&&foxRow==alphaRow+1,"Native dropdown fixture has two adjacent registered artist choices");
    SendMessageW(choice,CB_SETCURSEL,alphaRow,0);notify(pane,1006,CBN_SELCHANGE);
    NativeArtistChoice observed{choice};
    require(SetWindowSubclass(GetParent(choice),observeNativeArtistChoice,91,reinterpret_cast<DWORD_PTR>(&observed))!=FALSE,
        "Native artist notifications can be observed before Studio handles them");
    SendMessageW(choice,WM_KEYDOWN,VK_DOWN,0);SendMessageW(choice,WM_KEYUP,VK_DOWN,0);
    RemoveWindowSubclass(GetParent(choice),observeNativeArtistChoice,91);
    require(observed.changes>0&&SendMessageW(choice,CB_GETCURSEL,0,0)==foxRow&&text(choice)==fox.name,
        "A real native keyboard selection commits the second artist through CBN_SELCHANGE");
    std::wcout<<L"Native artist notification edit text: "<<observed.textAtChange<<L" -> "<<text(choice)<<L'\n';
    const auto draft=text(control(pane,liveExample));
    require(draft==L"Custom-fox-token_001_v01_aaa.png",
        "Live example uses the newly selected artist ID and its filename token before native edit text commits");
    click(pane,1041);
    require(wait([&]{return !studio.busy()&&studio.queuedFileCount()==1;})&&queueNames(pane).front().second==draft,
        "Immediately approving a native artist choice freezes the exact filename shown in its live example");
    const auto saved=std::ranges::find(studio.snapshot().files,file.id,&MediaRecord::id);
    require(saved!=studio.snapshot().files.end(),"Approved native dropdown source retains its original media identity");
    const auto assigned=std::ranges::find(studio.snapshot().artworks,saved->artworkId,&Artwork::id);
    require(assigned!=studio.snapshot().artworks.end()&&assigned->artists==std::vector<LibraryId>{fox.id}&&assigned->title==L"aaa",
        "Native dropdown assignment keeps the exact registered ID and strips only an implicit formatted source prefix");

    SendMessageW(control(pane,1009),CB_SETCURSEL,2,0);notify(pane,1009,CBN_SELCHANGE);
    const auto existingLabel=std::to_wstring(old.number)+L" · "+old.title;
    const auto existingRow=SendMessageW(control(pane,1008),CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(existingLabel.c_str()));
    require(existingRow>=0,"An existing artwork with an independent stored user title can be selected");
    SendMessageW(control(pane,1008),CB_SETCURSEL,existingRow,0);notify(pane,1008,CBN_SELCHANGE);
    SetWindowTextW(choice,alpha.name.c_str());notify(pane,1006,CBN_EDITCHANGE);
    SetWindowTextW(control(pane,1007),L"glimfox_009_v04_Existing Manual");
    const auto existingManual=text(control(pane,liveExample));
    require(existingManual==L"Custom-Худ1_006_v01_glimfox_009_v04_Existing Manual.png",
        "An explicit title overrides the existing artwork's stored user title in the live example");
    click(pane,1041);require(wait([&]{return !studio.busy();})&&queueNames(pane).front().second==existingManual,
        "Existing-work assignment queues the exact manual title displayed in its live example");
    const auto changedWork=std::ranges::find(studio.snapshot().artworks,old.id,&Artwork::id);
    require(changedWork!=studio.snapshot().artworks.end()&&changedWork->title==L"glimfox_009_v04_Existing Manual"&&changedWork->userTitle.empty(),
        "Saving an explicit existing-work title clears the previous user-title override");
    SendMessageW(control(pane,1009),CB_SETCURSEL,0,0);notify(pane,1009,CBN_SELCHANGE);

    SetWindowTextW(choice,alpha.name.c_str());notify(pane,1006,CBN_EDITCHANGE);
    SetWindowTextW(control(pane,1007),L"glimfox_007_v03_Manual");
    const auto manual=text(control(pane,liveExample));
    require(manual==L"Custom-Худ1_007_v01_glimfox_007_v03_Manual.png",
        "Typing another artist clears the dropdown binding while preserving explicit manual title and current template");
    click(pane,1041);require(wait([&]{return !studio.busy();})&&queueNames(pane).front().second==manual,
        "Typed artist edits produce the same approved filename as their live example");

    const auto other=folder/L"glimfox_bbb.png";
    {std::ofstream output(other,std::ios::binary);output<<"second shared-work source";}
    studio.setFiles({source,other},{source,other});require(wait([&]{return !studio.busy();}),"Two differently named sources are prepared for one shared artwork");
    SendMessageW(control(pane,1009),CB_SETCURSEL,1,0);notify(pane,1009,CBN_SELCHANGE);
    SendMessageW(choice,CB_SETCURSEL,alphaRow,0);notify(pane,1006,CBN_SELCHANGE);
    SendMessageW(choice,WM_KEYDOWN,VK_DOWN,0);SendMessageW(choice,WM_KEYUP,VK_DOWN,0);
    const auto shared=text(control(pane,liveExample));click(pane,1041);
    require(wait([&]{return !studio.busy();})&&studio.queuedFileCount()==2&&queueNames(pane).front().second==shared&&
        std::ranges::all_of(queueNames(pane),[](const auto& row){return row.second.ends_with(L"_aaa.png");}),
        "A blank shared-work title uses the same inferred first-source title in preview and actual batch");

    const auto custom=templatePane(studio);
    while(SendMessageW(control(custom,1021),LB_GETCOUNT,0,0)>0) {
        SendMessageW(control(custom,1021),LB_SETCURSEL,0,0);click(custom,1024);
    }
    SendMessageW(control(custom,1022),CB_SETCURSEL,static_cast<WPARAM>(NameBlockKind::original),0);click(custom,1023);click(custom,1041);pump();
    const auto intentional=text(control(pane,liveExample));
    require(intentional==source.filename().wstring(),"A custom Original block intentionally keeps the complete original filename");
    click(pane,1041);require(wait([&]{return !studio.busy();})&&queueNames(pane).front().second==intentional&&
        std::ranges::all_of(queueNames(pane),[](const auto& row){return row.first==row.second;}),
        "Smart implicit titles do not strip custom Original template output");
    const auto unlinked=folder/L"glimfox_ccc.png";
    {std::ofstream output(unlinked,std::ios::binary);output<<"unlinked artist-only source";}
    studio.setFiles({source,other,unlinked},{unlinked});require(wait([&]{return !studio.busy();}),"A source without an existing artwork can enter artist-only assignment");
    SendMessageW(control(pane,1009),CB_SETCURSEL,3,0);notify(pane,1009,CBN_SELCHANGE);click(pane,1041);
    require(wait([&]{return !studio.busy();}),"Artist-only assignment prepares the previously unlinked source");
    const auto newFile=std::ranges::find(studio.snapshot().files,unlinked,&MediaRecord::path);
    require(newFile!=studio.snapshot().files.end(),"Artist-only batch retains the unlinked source identity");
    const auto newWork=std::ranges::find(studio.snapshot().artworks,newFile->artworkId,&Artwork::id);
    require(newWork!=studio.snapshot().artworks.end()&&newWork->title==L"ccc"&&newWork->artists==std::vector<LibraryId>{fox.id},
        "Artist-only assignment infers a clean title only when the source has no existing artwork");
    studio.hide();
}

void checkBatchEventsAndArtistButton(HWND host,HFONT font,const fs::path& folder) {
    const auto source=folder/L"batch-event.png",database=folder/L"batch-events.sqlite3";
    {std::ofstream output(source,std::ios::binary);output<<"batch notification bytes";}
    CollectionStore registry(database);
    Artist first;first.name=L"First Artist";first.aliases=L"Shared Alias";registry.saveArtist(first);
    Artist second;second.name=L"Second Artist";second.aliases=L"Shared Alias";registry.saveArtist(second);
    RenameStudio studio(host,changed,[]{},database);
    RECT area{};GetClientRect(host,&area);studio.show(area,96,font);studio.setFiles({source},{source});
    require(wait([&]{return !studio.busy();})&&!studio.takeBatchQueued(),"Initial indexing never reports a newly approved batch");
    const auto pane=studio.window(),button=control(pane,RenameStudio::artistsCommandId);
    require(button&&IsWindowVisible(button)&&text(button)==L"База художников",
        "Batch artist field has a compact registry button with a readable accessible name");
    const auto requests=artistRegistryRequests;click(pane,RenameStudio::artistsCommandId);
    require(wait([&]{return artistRegistryRequests==requests+1;})&&!studio.takeBatchQueued()&&studio.queuedFileCount()==0,
        "Artist registry button delegates one owner command without changing the pending batch");

    SetWindowTextW(control(pane,1006),L"Shared Alias");click(pane,1041);
    require(studio.busy()&&wait([&]{return !studio.busy();})&&!studio.takeBatchQueued()&&studio.queuedFileCount()==0,
        "An asynchronous metadata failure does not report a successfully approved batch");
    SetWindowTextW(control(pane,1006),L"Batch Artist");SetWindowTextW(control(pane,1013),L"tag=first\r\ntag=second");click(pane,1041);
    require(!studio.busy()&&!studio.takeBatchQueued()&&studio.queuedFileCount()==0,
        "Input validation failure does not report a queued batch");
    SetWindowTextW(control(pane,1013),L"");click(pane,1041);studio.cancel();
    require(wait([&]{return !studio.busy();})&&!studio.takeBatchQueued(),"A cancelled preparation never reports a completed batch");
    SetWindowTextW(control(pane,1007),L"Late Cancelled Draft");click(pane,1041);
    // Leave the worker's completion posted while the UI still accepts Cancel.
    MSG completion{};bool completed=false;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<until) {
        if(PeekMessageW(&completion,pane,WM_APP+431,WM_APP+431,PM_NOREMOVE)) {completed=true;break;}
        Sleep(5);
    }
    require(completed,"Batch preparation finishes before the late-cancellation regression");
    studio.cancel();
    require(wait([&]{return !studio.busy();})&&!studio.takeBatchQueued()&&studio.queuedFileCount()==0,
        "Cancellation before accepting a completed batch preserves checks by keeping the old queue");
    SetWindowTextW(control(pane,1007),L"Approved Draft");click(pane,1041);
    require(wait([&]{return !studio.busy()&&studio.queuedFileCount()==1;})&&studio.takeBatchQueued()&&!studio.takeBatchQueued(),
        "Successful batch approval emits one consumed-once event");
    const auto frozen=queueNames(pane);
    click(pane,1012);require(wait([&]{return !studio.busy();})&&!studio.takeBatchQueued()&&queueNames(pane)==frozen,
        "Saving only metadata preserves approved queue names and emits no batch event");
    studio.refreshCollection();require(wait([&]{return !studio.busy();})&&!studio.takeBatchQueued()&&queueNames(pane)==frozen,
        "Read-only registry refresh cannot repeat a consumed batch event");
    SetWindowTextW(control(pane,1007),L"Replacement Draft");click(pane,1041);
    require(wait([&]{return !studio.busy()&&studio.queuedFileCount()==1;})&&studio.takeBatchQueued()&&!studio.takeBatchQueued()&&
        queueNames(pane)!=frozen,"Replacing an approved source emits one event without duplicating its queue row");
    const auto replaced=queueNames(pane);studio.setFiles({source},{});click(pane,1041);
    require(!studio.busy()&&!studio.takeBatchQueued()&&queueNames(pane)==replaced,
        "Attempting another batch without a selection preserves approved rows and emits no stale event");
    studio.hide();
}

void checkArtistOnlyVersionPreview(HWND host,HFONT font,const fs::path& folder) {
    const auto fresh=folder/L"Artist_Fresh.png",linkless=folder/L"Artist_Linkless.png",database=folder/L"artist-only-versions.sqlite3";
    {std::ofstream output(fresh,std::ios::binary);output<<"unlinked version source";}
    {std::ofstream output(linkless,std::ios::binary);output<<"existing artwork without a linked version";}
    CollectionStore registry(database);
    Artist artist;artist.name=L"Version Artist";artist.fileName=L"Artist";artist.id=registry.saveArtist(artist);
    auto files=registry.indexFiles({fresh,linkless});
    Artwork work;work.artists={artist.id};work.number=7;work.title=L"Existing Work";work.id=registry.saveArtwork(work);
    for(const auto number:{2LL,7LL}) {MediaVersion version;version.artworkId=work.id;version.number=number;registry.saveVersion(version);}
    files[1].artworkId=work.id;registry.saveFile(files[1]);
    RenameStudio studio(host,changed,[]{},database);RECT area{};GetClientRect(host,&area);studio.show(area,96,font);
    studio.setFiles({fresh,linkless},{fresh});require(wait([&]{return !studio.busy();}),"Artist-only version fixture loads unlinked and linkless sources");
    const auto pane=studio.window();SetWindowTextW(control(pane,1006),artist.name.c_str());notify(pane,1006,CBN_EDITCHANGE);
    SendMessageW(control(pane,1009),CB_SETCURSEL,3,0);notify(pane,1009,CBN_SELCHANGE);
    SetWindowTextW(control(pane,1011),L"5");notify(pane,1006,CBN_EDITCHANGE);
    const auto newWorkPreview=text(control(pane,liveExample));
    require(newWorkPreview.ends_with(L"_v01_Fresh.png"),"Artist-only preview starts an unlinked artwork at v01 despite a first-version input of five");
    click(pane,1041);require(wait([&]{return !studio.busy();})&&queueNames(pane).front().second==newWorkPreview,
        "Unlinked artist-only batch queues the exact v01 filename shown in its preview");
    studio.setFiles({fresh,linkless},{linkless});SetWindowTextW(control(pane,1007),L"Do Not Replace Existing Title");
    const auto nextPreview=text(control(pane,liveExample));
    require(nextPreview==L"Artist_007_v08_Existing Work.png",
        "Artist-only preview finds the existing artwork's next version when the file has no linked version");
    click(pane,1041);require(wait([&]{return !studio.busy();})&&std::ranges::any_of(queueNames(pane),[&](const auto& row){
        return row.first==linkless.filename().wstring()&&row.second==nextPreview;
    }),"Linkless existing-artwork batch queues the exact next-version filename shown in preview");
    MediaVersion later;later.artworkId=work.id;later.number=11;registry.saveVersion(later);studio.refreshCollection();
    require(wait([&]{return !studio.busy();}),"A later unrelated version can be loaded without replacing a linked version");
    SetWindowTextW(control(pane,1011),L"3");notify(pane,1006,CBN_EDITCHANGE);
    require(text(control(pane,liveExample))==nextPreview,"Artist-only preview preserves the file's valid linked version despite a different input and newer versions");
    click(pane,1041);require(wait([&]{return !studio.busy();})&&std::ranges::any_of(queueNames(pane),[&](const auto& row){
        return row.first==linkless.filename().wstring()&&row.second==nextPreview;
    }),"Artist-only reapproval preserves the linked version in both preview and queue");
    studio.hide();
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    const fs::path folder=fs::absolute(argv[1])/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto desktopName=L"VieweryStudioQA-"+std::to_wstring(GetCurrentProcessId());
    const HDESK previousDesktop=GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop=CreateDesktopW(desktopName.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop||!SetThreadDesktop(desktop)) {
        const DWORD error=GetLastError();if(desktop)CloseDesktop(desktop);
        std::cout<<"SKIP Private desktop unavailable (Win32 "<<error<<"). No UI or collection was opened.\n";return 77;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)) {SetThreadDesktop(previousDesktop);CloseDesktop(desktop);return 4;}
    HWND host=nullptr;HFONT font=nullptr;
    try {
        fs::create_directories(folder/L"first");fs::create_directories(folder/L"second");
        const INITCOMMONCONTROLSEX init{sizeof(init),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};InitCommonControlsEx(&init);
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=hostProcedure;wc.lpszClassName=L"Viewery.StudioQAHost.1";RegisterClassW(&wc);
        host=CreateWindowExW(0,wc.lpszClassName,L"Viewery Rename Studio",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,650,850,nullptr,nullptr,wc.hInstance,nullptr);
        require(host!=nullptr,"Private desktop host window is created");
        font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        checkArtistRegistryRefresh(host,font,folder);
        checkQueuedCycleUndo(host,font,folder);
        checkNativeArtistDropdown(host,font,folder);
        checkBatchEventsAndArtistButton(host,font,folder);
        checkArtistOnlyVersionPreview(host,font,folder);
        const auto one=folder/L"first/imageA01.png",two=folder/L"first/imageA02.png",keep=folder/L"first/keep.png";
        const auto three=folder/L"second/imageB01.png",four=folder/L"second/imageB02.png";
        {std::ofstream f(one,std::ios::binary);f<<"first bytes";}{std::ofstream f(two,std::ios::binary);f<<"second bytes";}
        {std::ofstream f(three,std::ios::binary);f<<"third bytes";}{std::ofstream f(four,std::ios::binary);f<<"fourth bytes";}
        {std::ofstream f(keep,std::ios::binary);f<<"unselected bytes";}
        unsigned beforeChanges=0;
        auto studio=std::make_unique<RenameStudio>(host,changed,[&]{++beforeChanges;},folder/L"collection.sqlite3");
        RECT area{};GetClientRect(host,&area);studio->show(area,96,font);
        studio->setFiles({one,two,keep},{one,two});
        require(studio->busy(),"Initial metadata indexing starts asynchronously");
        studio->openTemplates();
        require(!studio->templatesWindow(),"A template request during initial indexing is deferred");
        require(wait([&]{return !studio->busy()&&studio->snapshot().files.size()==3&&
            studio->templatesWindow()&&IsWindowVisible(studio->templatesWindow());}),
            "The first template click opens automatically when indexing finishes without a second click");
        unsigned editorWindows=0;
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM parameter)->BOOL {
            wchar_t name[80]{};GetClassNameW(window,name,static_cast<int>(std::size(name)));
            if(std::wstring_view(name)==L"Viewery.TemplateEditor.1") ++*reinterpret_cast<unsigned*>(parameter);
            return TRUE;
        },reinterpret_cast<LPARAM>(&editorWindows));
        require(editorWindows==1,"A deferred request creates exactly one owned template window");
        SendMessageW(studio->templatesWindow(),WM_CLOSE,0,0);pump();
        studio->setFiles({three,four},{three,four});studio->openTemplates();studio->cancel();
        require(wait([&]{return !studio->busy();})&&!IsWindowVisible(studio->templatesWindow()),
            "Cancel clears a deferred template request instead of reopening the window later");
        studio->setFiles({one,two,keep},{one,two});
        require(wait([&]{return !studio->busy();}),"A new selection resumes normal indexing after cancellation");
        RedrawWindow(studio->window(),nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        for(int iteration=0;iteration<20;++iteration) studio->show(area,96,font);
        require(!GetUpdateRect(studio->window(),nullptr,FALSE)&&!GetUpdateRect(control(studio->window(),1055),nullptr,FALSE),
            "Repeated unchanged show calls do not invalidate the panel during catalog zoom");
        const HWND pane=studio->window();
        require(!IsWindowVisible(control(pane,1013))&&!IsWindowVisible(control(pane,1014)),"Advanced metadata is hidden from the simple batch form");
        SetWindowTextW(control(pane,1006),L"Artist A");SetWindowTextW(control(pane,1007),L"First Light");
        SendMessageW(control(pane,1009),CB_SETCURSEL,1,0);notify(pane,1009,CBN_SELCHANGE);
        SetWindowTextW(control(pane,1013),L"tag=alpha\r\ntag=beta");click(pane,1041);
        require(!studio->busy()&&studio->snapshot().artists.empty()&&studio->queuedFileCount()==0,
            "Duplicate custom fields are rejected before creating a batch");
        SetWindowTextW(control(pane,1013),L"tag=alpha\r\nsource=artist=portal");
        require(text(control(pane,liveExample)).find(L"First Light")!=std::wstring::npos,"Simple data edits immediately update the draft filename example");
        const auto draftFirst=text(control(pane,liveExample));
        click(pane,1041);studio->setFiles({one,two,keep},{keep});
        require(wait([&]{return !studio->busy()&&studio->queuedFileCount()==2;}),"Approved first batch survives an immediate selection change while preparing");
        const auto firstNames=queueNames(pane);
        require(firstNames.front().second==draftFirst,"The live draft example exactly matches the first approved filename");
        require(firstNames.size()==2&&firstNames.front().second.find(L"Artist A_")==0&&fs::exists(one)&&fs::exists(two)&&beforeChanges==0,
            "Approving a batch freezes names without renaming files");
        studio->setFiles({three,four},{three,four});
        require(wait([&]{return !studio->busy()&&studio->snapshot().files.size()==5;}),"A second directory can be prepared while the first batch remains queued");
        SetWindowTextW(control(pane,1006),L"Artist B");SetWindowTextW(control(pane,1007),L"Second Work");
        SetWindowTextW(control(pane,1013),L"");click(pane,1041);
        require(wait([&]{return !studio->busy()&&studio->queuedFileCount()==4;}),"Different metadata produces a second batch in the same queue");
        const auto frozen=queueNames(pane);
        require(std::count_if(frozen.begin(),frozen.end(),[](const auto& item){return item.second.find(L"Artist A_")==0;})==2&&
            std::count_if(frozen.begin(),frozen.end(),[](const auto& item){return item.second.find(L"Artist B_")==0;})==2,
            "The queue preserves both batches with independent artist and title values");
        HWND editorPane=templatePane(*studio);
        require(editorPane!=nullptr&&IsWindowVisible(studio->templatesWindow()),"Templates are edited outside the batch panel");
        SendMessageW(control(editorPane,1021),LB_SETCURSEL,0,0);notify(editorPane,1021,LBN_SELCHANGE);
        SetWindowTextW(control(editorPane,1029),L"X-");click(editorPane,1033);
        SetWindowTextW(control(editorPane,1015),L"Batch preset");click(editorPane,1016);
        require(wait([&]{return !studio->busy();}),"A preset saves from the separate editor");
        require(queueNames(pane)==frozen,"Editing and saving a template never changes already approved batch names");
        checkOwnedEditor(*studio,folder);
        require(queueNames(pane)==frozen,"Selecting and inspecting templates leaves every queued name frozen");
        studio->setFiles({one,two,keep},{one});require(wait([&]{return !studio->busy();}),"A source can be approved again as a later batch");
        SetWindowTextW(control(pane,1006),L"Artist A");SendMessageW(control(pane,1009),CB_SETCURSEL,3,0);click(pane,1041);
        require(wait([&]{return !studio->busy()&&studio->queuedFileCount()==4;}),"A later batch replaces its existing source without duplication");
        require(queueNames(pane).front().second.find(L"X-Artist A_")==0&&queueNames(pane)[1]==frozen[1],
            "Re-approving one source updates only that source while the earlier batch stays frozen");
        click(pane,1002);
        chooseRow(pane,0);SetWindowTextW(control(pane,1039),L"collision.png");click(pane,1040);require(wait([&]{return !studio->busy();}),"The first queued name can be edited");
        chooseRow(pane,1);SetWindowTextW(control(pane,1039),L"collision.png");click(pane,1040);require(wait([&]{return !studio->busy();}),"The second queued name can be edited");
        require(cell(control(pane,1037),0,2)!=L"Готов"&&cell(control(pane,1037),1,2)!=L"Готов","Name conflicts are checked across separately approved batches");
        click(pane,1042);
        require(!studio->busy()&&beforeChanges==0&&fs::exists(one)&&fs::exists(two),"A conflicting queue cannot reach filesystem mutation");
        for(int row=0;row<2;++row) {chooseRow(pane,row);SetWindowTextW(control(pane,1039),frozen[static_cast<std::size_t>(row)].second.c_str());click(pane,1040);require(wait([&]{return !studio->busy();}),"A conflict can be corrected in the queue");}
        ListView_SetCheckState(control(pane,1037),0,FALSE);pump();
        require(studio->queuedFileCount()==4&&!ListView_GetCheckState(control(pane,1037),0),"Excluding a row retains it in the queue for later");
        ListView_SetCheckState(control(pane,1037),0,TRUE);pump();
        studio->setFiles({one,two,keep},{one});require(wait([&]{return !studio->busy();}),"Returning to a previous directory retains both batches");
        SetWindowTextW(control(pane,1006),L"Artist A");SetWindowTextW(control(pane,1007),L"");SendMessageW(control(pane,1009),CB_SETCURSEL,3,0);
        editorPane=templatePane(*studio);SendMessageW(control(editorPane,1021),LB_SETCURSEL,0,0);notify(editorPane,1021,LBN_SELCHANGE);
        SetWindowTextW(control(editorPane,1029),L"");click(editorPane,1033);click(editorPane,1041);pump();
        click(pane,1041);
        require(wait([&]{return !studio->busy()&&studio->queuedFileCount()==4;}),"Adding an already queued source replaces its row instead of duplicating it");
        chooseRow(pane,3);click(pane,1059);
        require(studio->queuedFileCount()==3,"An individual queued row can be removed");
        studio->setFiles({three,four},{four});require(wait([&]{return !studio->busy();}),"The removed source can be selected again");
        SetWindowTextW(control(pane,1006),L"Artist B");click(pane,1041);
        require(wait([&]{return !studio->busy()&&studio->queuedFileCount()==4;}),"A removed row can be approved again without affecting the other batch");
        studio->setFiles({three,four},{});pump();
        const auto ready=queueNames(pane);click(pane,1002);
        const auto requestsBeforeQueueClose=closeRequests;click(pane,closeStudio);
        require(wait([&]{return closeRequests==requestsBeforeQueueClose+1;})&&studio->queuedFileCount()==4,
            "Closing the rename panel preserves every approved batch");
        studio->show(area,96,font);click(pane,1002);
        require(queueNames(pane)==ready,"Reopening restores the exact approved queue names");
        require(screenshot(pane,folder/L"rename-studio-preview.png"),"The multi-batch queue screenshot is saved");
        {
            auto confirm=confirmOwnDialog(GetCurrentThreadId());click(pane,1042);
        }
        require(wait([&]{return !studio->busy()&&studio->queuedFileCount()==0;}),"One confirmation applies all approved batches even when no files are currently selected");
        std::vector<fs::path> renamed;
        const std::vector<fs::path> originals{one,two,three,four};
        const std::vector<std::string> contents{"first bytes","second bytes","third bytes","fourth bytes"};
        for(const auto& original:originals) {
            const auto name=std::ranges::find(ready,original.filename().wstring(),&std::pair<std::wstring,std::wstring>::first);
            require(name!=ready.end(),"Every original source remains represented exactly once");
            const auto target=original.parent_path()/name->second;renamed.push_back(target);
            require(fs::exists(target)&&!fs::exists(original),"The approved target exists after the single rename operation");
        }
        for(std::size_t i=0;i<renamed.size();++i) require(content(renamed[i])==contents[i],"Batch renaming preserves file contents");
        require(beforeChanges==1&&content(keep)=="unselected bytes","One operation changes only the four approved sources");
        studio->takeFileChanges();studio->setFiles({renamed[0],renamed[1],keep},{});require(wait([&]{return !studio->busy();}),"The collection refreshes renamed identities");
        click(pane,1003);require(ListView_GetItemCount(control(pane,1043))==1,"All batches produce one durable history operation");
        ListView_SetItemState(control(pane,1043),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);pump();
        require(wait([&]{return !studio->busy();})&&text(control(pane,1050)).find(L"imageA01.png")!=std::wstring::npos,"History exposes the paths of the entire combined operation");
        {auto confirm=confirmOwnDialog(GetCurrentThreadId());click(pane,1044);}
        require(wait([&]{return !studio->busy()&&std::ranges::all_of(originals,[](const fs::path& path){return fs::exists(path);});}),"Undo restores both batches from the single journal operation");
        for(std::size_t i=0;i<originals.size();++i) require(content(originals[i])==contents[i],"Undo preserves every original byte sequence");
        require(beforeChanges==2,"Undo invokes one file-change operation for all batches");
        studio->setFiles({one,two,keep},{one,two});require(wait([&]{return !studio->busy();}),"Panel remains usable after multi-directory Undo");
        checkScrollingLayout(*studio,host,folder);
        RECT originalClient{};GetClientRect(host,&originalClient);studio->show(originalClient,96,font);click(pane,1000);
        require(screenshot(pane,folder/L"rename-batch-data.png"),"The simplified batch data form screenshot is saved");
        const auto closeBefore=closeRequests;click(pane,closeStudio);
        require(wait([&]{return closeRequests==closeBefore+1;})&&!IsWindowVisible(pane),"Closing the panel keeps its collection intact");
        studio->show(originalClient,96,font);require(studio->snapshot().files.size()==5,"Reopening the batch panel restores the same collection");
        const auto artistControl=control(pane,1006);COMBOBOXINFO comboInfo{sizeof(comboInfo)};
        require(GetComboBoxInfo(artistControl,&comboInfo)&&comboInfo.hwndItem,"Artist input has an editable native field");
        SetFocus(comboInfo.hwndItem);SendMessageW(comboInfo.hwndItem,WM_KEYDOWN,VK_ESCAPE,0);
        require(wait([&]{return closeRequests==closeBefore+2;})&&!IsWindowVisible(pane),"Escape closes the batch panel from a nested input");
        std::cout<<"Studio batch and template checks passed.\n";
        studio.reset();DeleteObject(font);font=nullptr;DestroyWindow(host);host=nullptr;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<std::endl;if(host)DestroyWindow(host);if(font)DeleteObject(font);
        CoUninitialize();SetThreadDesktop(previousDesktop);CloseDesktop(desktop);return 1;
    }
    CoUninitialize();SetThreadDesktop(previousDesktop);CloseDesktop(desktop);return 0;
}
