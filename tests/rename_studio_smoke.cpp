#include "rename_studio.hpp"
#include "rename_template.hpp"
#include "frame_image.hpp"

#include <windows.h>
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

void click(HWND pane,int id) { SendMessageW(pane,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(pane,id))); }

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

bool screenshot(HWND host,const fs::path& output) {
    RECT rect{};GetClientRect(host,&rect);
    CapturedFrame frame;frame.width=rect.right;frame.height=rect.bottom;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=rect.right;
    info.bmiHeader.biHeight=-rect.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap||!pixels) {if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return false;}
    const auto old=SelectObject(dc,bitmap);
    const bool rendered=PrintWindow(host,dc,2)!=FALSE;
    if(rendered) {
        const auto* begin=static_cast<std::uint8_t*>(pixels);
        frame.bgra.assign(begin,begin+static_cast<std::size_t>(rect.right)*rect.bottom*4);
        for(std::size_t i=3;i<frame.bgra.size();i+=4) frame.bgra[i]=255;
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    return rendered&&saveFramePng(output,frame);
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
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;
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
        SetWindowTextW(GetDlgItem(pane,1006),L"Artist A");
        SetWindowTextW(GetDlgItem(pane,1007),L"First Light");
        SendMessageW(GetDlgItem(pane,1009),CB_SETCURSEL,1,0);
        SetWindowTextW(GetDlgItem(pane,1013),L"tag=alpha\r\ntag=beta");
        click(pane,1012);
        require(!studio->busy()&&studio->snapshot().artists.empty()&&text(GetDlgItem(pane,1005)).find(L"more than once")!=std::wstring::npos,
                "Duplicate custom field names are rejected before mutation");
        SetWindowTextW(GetDlgItem(pane,1013),L"tag=alpha\r\nsource=artist=portal");
        click(pane,1012);
        require(wait([&]{return !studio->busy()&&studio->snapshot().artworks.size()==1&&studio->snapshot().versions.size()==2;}),
                "Metadata assignment groups selected files into versions of one work");
        require(std::count_if(studio->snapshot().files.begin(),studio->snapshot().files.end(),[](const MediaRecord& file){
            return !file.artworkId.empty()&&file.fields.contains(L"source")&&file.fields.at(L"source")==L"artist=portal";})==2,
                "Multiline fields preserve values including equals signs and affect selected files only");
        const auto originalWork=studio->snapshot().artworks.front().id;
        const auto originalVersion=studio->snapshot().files.front().versionId;
        SetWindowTextW(GetDlgItem(pane,1006),L"Artist B");SetWindowTextW(GetDlgItem(pane,1007),L"");
        SendMessageW(GetDlgItem(pane,1009),CB_SETCURSEL,3,0);click(pane,1012);
        require(wait([&]{return !studio->busy();})&&studio->snapshot().artworks.size()==1&&studio->snapshot().versions.size()==2&&
                studio->snapshot().artworks.front().id==originalWork&&studio->snapshot().files.front().versionId==originalVersion,
                "Artist-only assignment preserves work and version identities and numbering");
        SetWindowTextW(GetDlgItem(pane,1006),L"Artist A");click(pane,1012);
        require(wait([&]{return !studio->busy();}),"Artist-only assignment can restore the original artist");
        click(pane,1001);
        SetWindowTextW(GetDlgItem(pane,1034),L" + ");SetWindowTextW(GetDlgItem(pane,1035),L"8");
        SendMessageW(GetDlgItem(pane,1036),CB_SETCURSEL,1,0);
        SendMessageW(GetDlgItem(pane,1021),LB_SETCURSEL,0,0);
        SendMessageW(pane,WM_COMMAND,MAKEWPARAM(1021,LBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(pane,1021)));
        SetWindowTextW(GetDlgItem(pane,1029),L"A-");click(pane,1033);
        require(text(GetDlgItem(pane,1034))==L" + "&&text(GetDlgItem(pane,1035))==L"8"&&
                SendMessageW(GetDlgItem(pane,1036),CB_GETCURSEL,0,0)==1,"Editing a block preserves unsaved global template settings");
        SetWindowTextW(GetDlgItem(pane,1015),L"Smoke preset");click(pane,1016);
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
        require(screenshot(pane,folder/L"rename-template.png"),"Native visual template screenshot is saved");
        click(pane,1002);click(pane,1041);
        studio->setFiles({one,two,three},{three});
        require(wait([&]{return !studio->busy();})&&ListView_GetItemCount(GetDlgItem(pane,1037))==0,
                "Changing selection discards an old asynchronous rename preview");
        studio->setFiles({one,two,three},{one,two});click(pane,1041);
        require(wait([&]{return !studio->busy()&&ListView_GetItemCount(GetDlgItem(pane,1037))==2;}),
                "New selection produces a two-file rename preview");
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
        require(ListView_GetItemCount(GetDlgItem(pane,1043))==1,"History lists the completed rename");
        ListView_SetItemState(GetDlgItem(pane,1043),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        require(wait([&]{return !studio->busy();})&&text(GetDlgItem(pane,1050)).find(L"image001.png")!=std::wstring::npos,"History exposes old and new paths");
        {
            auto confirm=confirmOwnDialog(GetCurrentThreadId());click(pane,1044);
        }
        require(wait([&]{return !studio->busy()&&fs::exists(one)&&fs::exists(two);}),"Undo restores original filenames through the native history panel");
        require(content(one)=="first bytes"&&content(two)=="second bytes"&&beforeChanges==2,
                "Undo preserves both file contents");
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




