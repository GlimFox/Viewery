#include "artist_registry.hpp"
#include "frame_image.hpp"

#include <commctrl.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs=std::filesystem;
using namespace viewery;
namespace {
int checks=0;
LRESULT CALLBACK hostProc(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR value) {
    if(message==WM_APP+700) {++*reinterpret_cast<int*>(value);return 0;}return DefSubclassProc(window,message,w,l);
}
void require(bool condition,const char* message) {
    ++checks;std::cout<<(condition?"OK ":"FAIL ")<<message<<std::endl;
    if(!condition) throw std::runtime_error(message);
}
void pump() {
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
}
bool wait(auto predicate) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    do {pump();if(predicate())return true;Sleep(10);} while(std::chrono::steady_clock::now()<end);
    return false;
}
HWND control(HWND parent,int id) {
    struct Search {int id;HWND window=nullptr;} search{id};
    EnumChildWindows(parent,[](HWND child,LPARAM value)->BOOL {
        auto& item=*reinterpret_cast<Search*>(value);
        if(GetDlgCtrlID(child)==item.id) {item.window=child;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));return search.window;
}
void click(HWND parent,int id) {SendMessageW(control(parent,id),BM_CLICK,0,0);pump();}
void edit(HWND parent,int id,const wchar_t* value) {SetWindowTextW(control(parent,id),value);pump();}
std::wstring text(HWND window) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window))+1,L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(window,value.data(),static_cast<int>(value.size()))));return value;
}
RECT bounds(HWND parent,HWND child) {
    RECT area{};GetWindowRect(child,&area);MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&area),2);return area;
}
std::string bytes(const fs::path& file) {std::ifstream stream(file,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
std::jthread answerOwnDialog(DWORD thread,int answer) {
    return std::jthread([thread,answer](std::stop_token stop) {
        while(!stop.stop_requested()) {
            struct Query {HWND dialog=nullptr;} query;
            EnumThreadWindows(thread,[](HWND window,LPARAM value)->BOOL {
                wchar_t type[40]{};GetClassNameW(window,type,40);
                if(wcscmp(type,L"#32770")==0&&GetDlgItem(window,IDYES)&&IsWindowVisible(window)) {
                    reinterpret_cast<Query*>(value)->dialog=window;return FALSE;
                }return TRUE;
            },reinterpret_cast<LPARAM>(&query));
            if(query.dialog) {PostMessageW(query.dialog,WM_COMMAND,MAKEWPARAM(answer,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(query.dialog,answer)));return;}
            Sleep(10);
        }
    });
}
void selectArtist(HWND pane,const wchar_t* name) {
    const auto list=control(pane,2307);bool found=false;
    for(int row=0;row<ListView_GetItemCount(list);++row) {
        wchar_t value[256]{};ListView_GetItemText(list,row,0,value,256);
        if(wcscmp(value,name)==0) {ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);found=true;break;}
    }
    require(found,"requested artist is selectable in the native registry list");pump();
}
bool capture(HWND window,const fs::path& path) {
    RECT area{};GetClientRect(window,&area);HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=area.right;
    info.bmiHeader.biHeight=-area.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HBITMAP image=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!image||!pixels) {if(image)DeleteObject(image);DeleteDC(dc);return false;}
    const auto old=SelectObject(dc,image);const bool ok=PrintWindow(window,dc,3)!=FALSE;CapturedFrame frame;
    if(ok) {frame.width=area.right;frame.height=area.bottom;const auto first=static_cast<const std::uint8_t*>(pixels);
        frame.bgra.assign(first,first+static_cast<std::size_t>(area.right)*area.bottom*4);
        for(std::size_t at=3;at<frame.bgra.size();at+=4)frame.bgra[at]=255;}
    SelectObject(dc,old);DeleteObject(image);DeleteDC(dc);return ok&&saveFramePng(path,frame);
}
void checkLayout(ArtistRegistry& registry,const fs::path& folder) {
    const auto pane=registry.window();
    for(const UINT dpi:{96U,120U,144U}) for(const int width:{500,700}) for(const int height:{500,740}) {
        registry.show(dpi);SetWindowPos(pane,nullptr,0,0,MulDiv(width,static_cast<int>(dpi),96),MulDiv(height,static_cast<int>(dpi),96),SWP_NOZORDER|SWP_NOACTIVATE);pump();
        click(pane,2300);const auto header=bounds(pane,control(pane,2300)),footer=bounds(pane,control(pane,2302));
        const auto viewport=control(pane,2326);SendMessageW(viewport,WM_VSCROLL,SB_BOTTOM,0);pump();
        const auto afterHeader=bounds(pane,control(pane,2300)),afterFooter=bounds(pane,control(pane,2302));
        require(EqualRect(&header,&afterHeader)&&EqualRect(&footer,&afterFooter),"scrolling keeps registry tabs and close action fixed");
        SendMessageW(viewport,WM_VSCROLL,SB_TOP,0);pump();RECT client{};GetClientRect(pane,&client);
        const auto body=bounds(pane,viewport);
        require(header.left>=0&&header.right<=client.right&&footer.right<=client.right&&footer.bottom<=client.bottom&&body.top>=header.bottom&&body.bottom<=footer.top,
            "registry body is clipped between fixed controls at every tested size and DPI");
        bool captionsFit=true;
        for(const int id:{2300,2301,2302,2308,2309,2314,2317,2318,2329}) {
            HWND button=control(pane,id);if(!IsWindowVisible(button))continue;
            const auto value=text(button);HDC dc=GetDC(button);const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(button,WM_GETFONT,0,0)));
            SIZE measured{};GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&measured);SelectObject(dc,old);ReleaseDC(button,dc);
            RECT available{};GetClientRect(button,&available);captionsFit=captionsFit&&measured.cx+MulDiv(12,static_cast<int>(dpi),96)<=available.right;
        }
        require(captionsFit,"registry button captions fit without truncation");
        if(width==500&&height==740)require(capture(pane,folder/(L"artists-"+std::to_wstring(dpi)+L".png")),"registry renders an isolated native UI snapshot");
    }
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto previous=GetThreadDesktop(GetCurrentThreadId());
    const auto desktopName=L"VieweryArtistTest-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(desktopName.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop||!SetThreadDesktop(desktop)) {
        const DWORD error=GetLastError();if(desktop)CloseDesktop(desktop);
        std::cout<<"SKIP Private desktop unavailable (Win32 "<<error<<"). No user collection opened.\n";return 77;
    }
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)) {SetThreadDesktop(previous);CloseDesktop(desktop);return 3;}
    int result=0;
    try {
        const auto folder=fs::absolute(argv[1])/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(folder);const auto database=folder/L"collection.sqlite3";
        const auto host=CreateWindowExW(0,L"STATIC",L"Artist registry test owner",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(host!=nullptr,"isolated test owner is created");
        int ownerNotifications=0;SetWindowSubclass(host,hostProc,1,reinterpret_cast<DWORD_PTR>(&ownerNotifications));
        {
            int beforeCount=0;ArtistRegistry registry(host,WM_APP+700,[&]{++beforeCount;},database);
            require(!fs::exists(database),"constructing the registry performs no database I/O");
            registry.show();require(wait([&]{return !registry.busy();}),"initial registry load completes");const auto pane=registry.window();
            require(ownerNotifications==0,"read-only loading does not notify the owner of data changes");
            require(IsWindowVisible(pane)&&(GetWindowLongPtrW(pane,GWL_STYLE)&WS_CHILD)==0,"artist registry is an owned resizable modeless window");
            edit(pane,2311,L"Худ1");edit(pane,2316,L"Alt");click(pane,2317);
            require(wait([&]{return !registry.busy();}),"new predefined artist saves asynchronously");
            CollectionStore db(database);auto snapshot=db.snapshot();
            require(snapshot.artists.size()==1&&snapshot.artists.front().name==L"Худ1","new artist persists before files are assigned");
            require(ownerNotifications==1,"successful mutation notifies the owner exactly once");
            const auto firstId=snapshot.artists.front().id;
            click(pane,2308);edit(pane,2311,L"Соавтор");click(pane,2317);require(wait([&]{return !registry.busy();}),"second artist saves");
            snapshot=db.snapshot();require(snapshot.artists.size()==2,"registry adds independent predefined artists");
            const auto second=std::ranges::find_if(snapshot.artists,[](const Artist& artist){return artist.name==L"Соавтор";})->id;
            edit(pane,2311,L"Несохранённое имя");edit(pane,2313,L"custom-token");edit(pane,2316,L"Draft alias");registry.refresh();
            require(wait([&]{return !registry.busy();})&&text(control(pane,2311))==L"Несохранённое имя"&&text(control(pane,2313))==L"custom-token"&&text(control(pane,2316))==L"Draft alias",
                "background refresh preserves unsaved existing artist fields");
            click(pane,2329);require(wait([&]{return !registry.busy();})&&text(control(pane,2311))==L"Соавтор","explicit Reload replaces the current editor draft");
            click(pane,2308);edit(pane,2311,L"Новый черновик");registry.refresh();
            require(wait([&]{return !registry.busy();})&&text(control(pane,2311))==L"Новый черновик"&&db.snapshot().artists.size()==2,
                "background refresh preserves an unsaved new artist without creating it");
            click(pane,2329);require(wait([&]{return !registry.busy();})&&text(control(pane,2311)).empty(),"explicit Reload can discard a new artist draft");
            selectArtist(pane,L"Соавтор");edit(pane,2311,L"Соавтор новый");click(pane,2317);
            require(wait([&]{return !registry.busy();}),"artist with no works can be renamed");
            const auto metadataOnly=registry.takeFileChanges();
            require(metadataOnly&&metadataOnly->success&&!metadataOnly->operationId.empty()&&metadataOnly->changes.empty(),"artist-only rename is journaled without file operations");
            require(db.undo(metadataOnly->operationId).success,"artist-only rename can be undone");registry.refresh();require(wait([&]{return !registry.busy();}),"registry refresh follows undo");
            click(pane,2308);edit(pane,2311,L"ХУД1");click(pane,2317);require(wait([&]{return !registry.busy();}),"duplicate artist attempt finishes");
            require(db.snapshot().artists.size()==2,"case-insensitive duplicate artist is rejected without adding data");

            CapturedFrame fixture;fixture.width=2;fixture.height=2;fixture.bgra={0,0,255,255,0,255,0,255,255,0,0,255,255,255,255,255};
            const std::vector<fs::path> paths={folder/L"Худ1_001_v01.png",folder/L"Alt_002_v01.png",folder/L"original.png"};
            for(const auto& file:paths)require(saveFramePng(file,fixture),"synthetic image fixture is valid PNG");const auto expectedBytes=bytes(paths.front());
            auto records=db.indexFiles(paths);Artwork work;work.artists={firstId,second};work.title=L"Shared artwork";work.number=1;const auto workId=db.saveArtwork(work);
            MediaVersion version;version.artworkId=workId;const auto versionId=db.saveVersion(version);std::vector<LibraryId> fileIds;for(const auto& record:records)fileIds.push_back(record.id);db.assignFiles(fileIds,workId,versionId);
            const auto beforeRefresh=ownerNotifications;
            registry.refresh();require(wait([&]{return !registry.busy();}),"associated work counts refresh");selectArtist(pane,L"Худ1");
            require(ownerNotifications==beforeRefresh,"refresh never creates a collection notification cycle");
            wchar_t count[40]{};const auto list=control(pane,2307);const auto row=ListView_GetNextItem(list,-1,LVNI_SELECTED);ListView_GetItemText(list,row,2,count,40);
            require(wcscmp(count,L"3")==0,"registry counts all linked files including original names");
            edit(pane,2311,L"Худ2");click(pane,2317);require(wait([&]{return !registry.busy();}),"renaming artist prepares a review before mutation");
            require(ListView_GetItemCount(control(pane,2320))==3&&IsWindowVisible(control(pane,2324)),"review includes changed and unchanged linked filenames");
            registry.refresh();require(wait([&]{return !registry.busy();})&&ListView_GetItemCount(control(pane,2320))==3&&IsWindowVisible(control(pane,2324))&&text(control(pane,2311))==L"Худ2",
                "background refresh preserves an unchanged artist review and draft");
            require(fs::exists(paths[0])&&fs::exists(paths[1])&&beforeCount==0,"preview does not rename files or release playback");
            require(ownerNotifications==beforeRefresh,"preview never notifies the owner of a collection mutation");
            {auto answer=answerOwnDialog(GetCurrentThreadId(),IDNO);click(pane,2324);}
            require(!registry.busy()&&beforeCount==0&&fs::exists(paths[0]),"declining concrete rename confirmation preserves files");
            snapshot=db.snapshot();auto expectedArtist=*std::ranges::find(snapshot.artists,firstId,&Artist::id);
            auto staleArtist=expectedArtist;staleArtist.aliases+=L";Other";db.saveArtist(staleArtist);
            {auto answer=answerOwnDialog(GetCurrentThreadId(),IDYES);click(pane,2324);}
            require(wait([&]{return !registry.busy();}),"stale reviewed rename finishes safely");const auto failed=registry.takeFileChanges();
            require(failed&&!failed->success&&failed->changes.empty()&&beforeCount==1&&ownerNotifications==beforeRefresh+1,
                "failed apply notifies owner after playback release and leaves files unchanged");
            require(fs::exists(paths[0])&&fs::exists(paths[1]),"stale artist review cannot move any file");
            db.saveArtist(expectedArtist);registry.refresh();require(wait([&]{return !registry.busy();}),"registry refreshes after stale review");selectArtist(pane,L"Худ1");
            edit(pane,2311,L"Худ2");click(pane,2317);require(wait([&]{return !registry.busy();}),"review is prepared for external metadata refresh check");
            db.saveArtist(staleArtist);registry.refresh();
            require(wait([&]{return !registry.busy();})&&text(control(pane,2311))==L"Худ2"&&ListView_GetItemCount(control(pane,2320))==0&&text(control(pane,2303)).find(L"извне")!=std::wstring::npos,
                "external artist change clears stale preview while preserving draft and expected original");
            db.saveArtist(expectedArtist);click(pane,2329);require(wait([&]{return !registry.busy();}),"explicit reload resolves externally changed artist editor");
            edit(pane,2311,L"Худ2");click(pane,2317);require(wait([&]{return !registry.busy();}),"fresh review is generated after stale metadata rejection");
            {auto answer=answerOwnDialog(GetCurrentThreadId(),IDYES);click(pane,2324);}
            require(wait([&]{return !registry.busy();}),"confirmed artist rename completes");const auto renamed=registry.takeFileChanges();
            require(renamed&&renamed->success&&renamed->changes.size()==2&&beforeCount==2,"rename result identifies changes and calls playback release once per apply");
            require(fs::exists(folder/L"Худ2_001_v01.png")&&fs::exists(folder/L"Худ2_002_v01.png")&&fs::exists(paths[2])&&!fs::exists(paths[0]),"name and aliases are updated while original filenames are preserved");
            require(bytes(folder/L"Худ2_001_v01.png")==expectedBytes&&bytes(folder/L"Худ2_002_v01.png")==expectedBytes&&bytes(paths[2])==expectedBytes,"artist rename preserves image contents");
            require(db.undo(renamed->operationId).success&&fs::exists(paths[0])&&fs::exists(paths[1]),"reviewed artist rename can restore original names");
            registry.refresh();require(wait([&]{return !registry.busy();}),"registry reloads restored artist metadata");selectArtist(pane,L"Худ1");

            checkLayout(registry,folder);click(pane,2300);selectArtist(pane,L"Худ1");
            {auto answer=answerOwnDialog(GetCurrentThreadId(),IDYES);click(pane,2309);}
            require(wait([&]{return !registry.busy();}),"confirmed artist deletion completes");snapshot=db.snapshot();
            require(snapshot.artists.size()==1&&snapshot.artists.front().id==second&&snapshot.artworks.front().artists==std::vector<LibraryId>{second},"deleting artist keeps coauthor and artwork links");
            require(std::ranges::all_of(paths,[](const fs::path& path){return fs::exists(path);})&&snapshot.files.size()==3,"deleting artist never deletes physical files or media records");
            registry.refresh();registry.cancel();require(wait([&]{return !registry.busy();}),"cancel request leaves asynchronous registry in usable state");
            SetFocus(control(pane,2306));MSG tab{};tab.hwnd=control(pane,2306);tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;
            require(registry.processMessage(tab)&&GetFocus()==control(pane,2307),"Tab moves from search to the artist list");
            MSG enter{};enter.hwnd=control(pane,2308);enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
            require(registry.processMessage(enter)&&text(control(pane,2311)).empty(),"Enter activates the focused Add action");
            MSG escape{};escape.hwnd=control(pane,2311);escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;
            require(registry.processMessage(escape)&&!IsWindowVisible(pane),"Escape closes registry from a nested editor");
            registry.show();SendMessageW(pane,WM_CLOSE,0,0);require(!IsWindowVisible(pane)&&IsWindow(pane),"closing preserves the reusable registry instance");
            std::cout<<"Native artist registry checks passed: "<<checks<<"\n";
        }
        DestroyWindow(host);pump();
    } catch(const std::exception& error) {std::cerr<<error.what()<<std::endl;result=1;}
    CoUninitialize();SetThreadDesktop(previous);CloseDesktop(desktop);return result;
}
