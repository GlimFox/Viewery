#include "settings_window.hpp"
#include "frame_image.hpp"

#include <objbase.h>
#include <commctrl.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs=std::filesystem;
using namespace viewery;

namespace {
int checks=0;
void require(bool condition,const char* label)
{
    ++checks;
    std::cout<<(condition?"OK ":"FAIL ")<<label<<std::endl;
    if(!condition) throw std::runtime_error(label);
}
void pump()
{
    MSG message{};
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&message);DispatchMessageW(&message);
    }
}
HWND control(HWND parent,int id)
{
    struct Search {int id;HWND result=nullptr;} search{id};
    EnumChildWindows(parent,[](HWND child,LPARAM value)->BOOL {
        auto& query=*reinterpret_cast<Search*>(value);
        if(GetDlgCtrlID(child)==query.id) {query.result=child;return FALSE;}
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.result;
}
void click(HWND parent,int id)
{
    HWND target=control(parent,id);
    SendMessageW(target,BM_CLICK,0,0);pump();
}
RECT bounds(HWND parent,HWND child)
{
    RECT result{};GetWindowRect(child,&result);
    MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&result),2);
    return result;
}
bool screenshot(HWND window,const fs::path& file)
{
    RECT area{};GetClientRect(window,&area);
    if(area.right<=0||area.bottom<=0) return false;
    HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO description{};
    description.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    description.bmiHeader.biWidth=area.right;description.bmiHeader.biHeight=-area.bottom;
    description.bmiHeader.biPlanes=1;description.bmiHeader.biBitCount=32;
    description.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HBITMAP image=CreateDIBSection(dc,&description,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!image||!pixels) {if(image)DeleteObject(image);DeleteDC(dc);return false;}
    const auto previous=SelectObject(dc,image);
    const bool rendered=PrintWindow(window,dc,3)!=FALSE;
    CapturedFrame frame;
    if(rendered) {
        frame.width=area.right;frame.height=area.bottom;
        const auto* begin=static_cast<std::uint8_t*>(pixels);
        frame.bgra.assign(begin,begin+static_cast<std::size_t>(area.right)*area.bottom*4);
        for(std::size_t at=3;at<frame.bgra.size();at+=4) frame.bgra[at]=255;
    }
    SelectObject(dc,previous);DeleteObject(image);DeleteDC(dc);
    return rendered&&saveFramePng(file,frame);
}
}

int wmain(int argc,wchar_t** argv)
{
    if(argc!=2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HDESK previous=GetThreadDesktop(GetCurrentThreadId());
    const auto name=L"ViewerySettingsTest-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop||!SetThreadDesktop(desktop)) {
        const DWORD error=GetLastError();if(desktop)CloseDesktop(desktop);
        std::cout<<"SKIP Private desktop unavailable (Win32 "<<error<<"). No user settings opened.\n";
        return 77;
    }
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized)) {SetThreadDesktop(previous);CloseDesktop(desktop);return 3;}
    int result=0;
    try {
        const auto folder=fs::absolute(argv[1])/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(folder);
        std::vector<SettingsEvent> events;
        SettingsWindow settings(nullptr,[&](const SettingsEvent& event){events.push_back(event);});
        SettingsState state;state.version=L"0.2.0-test";
        settings.show(state);pump();
        const HWND pane=settings.window();
        require(pane&&settings.visible(),"settings are a separate visible modeless window");
        require((GetWindowLongPtrW(pane,GWL_STYLE)&WS_CHILD)==0,"settings are not attached as a canvas child panel");
        require(SendMessageW(control(pane,2205),BM_GETCHECK,0,0)==BST_CHECKED,"module checkbox reflects supplied state");
        click(pane,2205);
        require(events.back().action==SettingsAction::renameModule&&events.back().value==0,"module checkbox emits explicit unchecked value");
        require(!IsWindowEnabled(control(pane,2206)),"template actions disable when module is off");
        require(!IsWindowEnabled(control(pane,2231)),"artist registry disables when module is off");
        state.renameEnabled=true;settings.update(state);pump();
        const auto eventsBeforeUpdate=events.size();settings.update(state);pump();
        require(events.size()==eventsBeforeUpdate,"state refresh does not emit settings actions");
        SetFocus(control(pane,2205));
        MSG tabKey{};tabKey.hwnd=control(pane,2205);tabKey.message=WM_KEYDOWN;tabKey.wParam=VK_TAB;
        require(settings.processMessage(tabKey)&&GetFocus()==control(pane,2206),"Tab skips static settings labels and reaches the next action");
        MSG enterKey{};enterKey.hwnd=control(pane,2206);enterKey.message=WM_KEYDOWN;enterKey.wParam=VK_RETURN;
        require(settings.processMessage(enterKey)&&events.back().action==SettingsAction::templates,"Enter activates a focused settings action");
        click(pane,2206);require(events.back().action==SettingsAction::templates,"template editor is delegated to the owner");
        click(pane,2231);require(events.back().action==SettingsAction::artists,"artist registry is delegated to the owner");
        click(pane,2207);require(events.back().action==SettingsAction::backupCollection,"database backup is delegated without touching data");
        click(pane,2208);require(events.back().action==SettingsAction::exportMetadata,"metadata export is delegated to the owner");
        click(pane,2209);require(events.back().action==SettingsAction::importMetadata,"metadata import is delegated to the owner");
        click(pane,2201);
        HWND quick=control(pane,2210);
        require(SendMessageW(quick,CB_GETCOUNT,0,0)==4,"Quick View exposes all four hotkey choices");
        SendMessageW(quick,CB_SETCURSEL,2,0);
        SendMessageW(GetParent(quick),WM_COMMAND,MAKEWPARAM(2210,CBN_SELCHANGE),reinterpret_cast<LPARAM>(quick));
        require(events.back().action==SettingsAction::quickHotkey&&events.back().value==2,"Quick View choice emits the requested hotkey");
        HWND group=control(pane,2211);SendMessageW(group,CB_SETCURSEL,1,0);
        SendMessageW(GetParent(group),WM_COMMAND,MAKEWPARAM(2211,CBN_SELCHANGE),reinterpret_cast<LPARAM>(group));
        require(events.back().action==SettingsAction::groupChord&&events.back().value==1,"group navigation emits the chosen chord");
        click(pane,2202);click(pane,2212);
        require(events.back().action==SettingsAction::automaticUpdates&&events.back().value==0,"automatic update checkbox emits explicit value");
        click(pane,2213);require(events.back().action==SettingsAction::checkUpdates,"manual update check stays separate from the automatic switch");
        state.canInstall=true;settings.update(state);click(pane,2214);
        require(events.back().action==SettingsAction::installUpdate,"update installation is delegated, never performed by the component");
        state.updateBusy=true;state.updateStatus=L"Проверка обновлений…";settings.update(state);
        require(!IsWindowEnabled(control(pane,2213))&&!IsWindowEnabled(control(pane,2214))&&!IsWindowEnabled(control(pane,2216)),"busy update state disables conflicting update actions");
        state.updateBusy=false;settings.update(state);click(pane,2215);
        require(events.back().action==SettingsAction::releaseNotes,"release notes are an owner action");
        click(pane,2216);require(events.back().action==SettingsAction::mediaTools,"media installation is delegated, not executed");
        click(pane,2203);click(pane,2219);
        require(events.back().action==SettingsAction::repository,"About links are delegated to the owner");
        for(const UINT dpi:{96U,120U,144U}) {
            for(const int width:{400,560}) for(const int height:{360,650}) {
                settings.show(state,dpi);
                SetWindowPos(pane,nullptr,0,0,MulDiv(width,static_cast<int>(dpi),96),MulDiv(height,static_cast<int>(dpi),96),SWP_NOZORDER|SWP_NOACTIVATE);
                for(const int tab:{2200,2201,2202,2203}) {
                    click(pane,tab);pump();
                    const auto header=bounds(pane,control(pane,2200));
                    const auto close=bounds(pane,control(pane,2204));
                    const HWND viewport=control(pane,2229);
                    // Viewport/content IDs follow the public action controls; locate
                    // the scroll host by style to keep geometry checks independent.
                    HWND scrollHost=nullptr;
                    for(HWND child=GetWindow(pane,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
                        if(GetWindowLongPtrW(child,GWL_STYLE)&WS_VSCROLL) {scrollHost=child;break;}
                    }
                    if(!scrollHost) scrollHost=viewport;
                    require(scrollHost!=nullptr,"settings have a distinct scroll viewport");
                    SendMessageW(scrollHost,WM_VSCROLL,SB_BOTTOM,0);pump();
                    const auto scrolledHeader=bounds(pane,control(pane,2200));
                    const auto scrolledClose=bounds(pane,control(pane,2204));
                    require(EqualRect(&header,&scrolledHeader)&&EqualRect(&close,&scrolledClose),"tabs and close button stay fixed while settings scroll");
                    SendMessageW(scrollHost,WM_VSCROLL,SB_TOP,0);pump();
                    RECT client{};GetClientRect(pane,&client);
                    require(header.right<=client.right&&close.right<=client.right&&close.bottom<=client.bottom,"fixed settings controls remain inside the resized client");
                    if(tab==2201) {
                        const HWND hint=control(pane,2224);RECT textRect{};GetClientRect(hint,&textRect);
                        const int allocated=textRect.bottom;textRect.bottom=0;
                        const int count=GetWindowTextLengthW(hint);std::wstring value(static_cast<std::size_t>(count)+1,L'\0');
                        GetWindowTextW(hint,value.data(),count+1);
                        HDC dc=GetDC(hint);const auto oldFont=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(hint,WM_GETFONT,0,0)));
                        DrawTextW(dc,value.c_str(),count,&textRect,DT_CALCRECT|DT_WORDBREAK|DT_EXTERNALLEADING|DT_NOPREFIX);
                        SelectObject(dc,oldFont);ReleaseDC(hint,dc);
                        require(textRect.bottom<=allocated,"all shortcut help lines fit vertically at the tested DPI");
                    }
                    for(const int id:{2200,2201,2202,2203,2204,2206,2207,2208,2209,2213,2214,2215,2216,2219}) {
                        HWND button=control(pane,id);
                        if(!IsWindowVisible(button)) continue;
                        wchar_t value[128]{};const int length=GetWindowTextW(button,value,128);
                        HDC dc=GetDC(button);const auto oldFont=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(button,WM_GETFONT,0,0)));
                        SIZE size{};GetTextExtentPoint32W(dc,value,length,&size);
                        SelectObject(dc,oldFont);ReleaseDC(button,dc);
                        RECT buttonRect{};GetClientRect(button,&buttonRect);
                        require(size.cx+MulDiv(12,static_cast<int>(dpi),96)<=buttonRect.right,"visible settings button captions fit at the tested width and DPI");
                    }
                }
            }
            settings.show(state,dpi);SetWindowPos(pane,nullptr,0,0,MulDiv(500,static_cast<int>(dpi),96),MulDiv(640,static_cast<int>(dpi),96),SWP_NOZORDER|SWP_NOACTIVATE);
            click(pane,2201);pump();
            require(screenshot(pane,folder/(L"settings-controls-"+std::to_wstring(dpi)+L".png")),"native settings snapshot is rendered");
        }
        settings.show(state);click(pane,2201);
        HWND choice=control(pane,2210);SendMessageW(choice,CB_SHOWDROPDOWN,TRUE,0);
        MSG escape{};escape.hwnd=choice;escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;
        require(!settings.processMessage(escape)&&settings.visible(),"Escape first belongs to an open choice list");
        SendMessageW(choice,WM_KEYDOWN,VK_ESCAPE,0);pump();
        require(settings.visible(),"closing a dropdown preserves the settings window");
        SendMessageW(choice,CB_SHOWDROPDOWN,FALSE,0);
        require(settings.processMessage(escape)&&!settings.visible(),"Escape closes modeless settings after the dropdown closes");
        settings.show(state);click(pane,2204);
        require(!settings.visible(),"close button hides the settings window");
        settings.show(state);SendMessageW(pane,WM_CLOSE,0,0);
        require(!settings.visible()&&IsWindow(pane),"native window close preserves the reusable settings instance");
        std::cout<<"Settings checks passed: "<<checks<<"\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    CoUninitialize();SetThreadDesktop(previous);CloseDesktop(desktop);return result;
}
