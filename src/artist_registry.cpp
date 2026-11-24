#include "artist_registry.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <map>
#include <set>
#include <thread>
#include <utility>

namespace viewery {
namespace {
constexpr COLORREF bg=RGB(23,26,33),input=RGB(36,40,52),fg=RGB(241,242,247),muted=RGB(149,155,170),accent=RGB(155,140,255);
constexpr UINT completed=WM_APP+620;
enum Id {
    tabRegistry=2300,tabFiles,closeButton,statusLabel,summaryLabel,searchLabel,searchEdit,artistList,newButton,deleteButton,
    nameLabel,nameEdit,fileNameLabel,fileNameEdit,advancedButton,aliasesLabel,aliasesEdit,saveButton,previewButton,
    previewLabel,previewList,editLabel,editName,editButton,applyButton,backButton,viewportId,contentId,cancelButton,reloadButton
};
void fill(HDC dc,RECT area,COLORREF color) {const auto brush=CreateSolidBrush(color);FillRect(dc,&area,brush);DeleteObject(brush);}
std::wstring text(HWND control) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control))+1,L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(control,value.data(),static_cast<int>(value.size()))));return value;
}
void setText(HWND control,const std::wstring& value) {if(text(control)!=value) SetWindowTextW(control,value.c_str());}
std::wstring trim(std::wstring value) {
    const auto first=value.find_first_not_of(L" \t\r\n");
    return first==std::wstring::npos?std::wstring{}:value.substr(first,value.find_last_not_of(L" \t\r\n")-first+1);
}
std::wstring wide(const char* value) {
    const int count=MultiByteToWideChar(CP_UTF8,0,value,-1,nullptr,0);
    if(count<1) return L"Не удалось выполнить действие";
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value,-1,result.data(),count);result.resize(static_cast<std::size_t>(count-1));return result;
}
bool contains(std::wstring value,std::wstring query) {
    const auto upper=[](std::wstring& item) {if(!item.empty()) CharUpperBuffW(item.data(),static_cast<DWORD>(item.size()));};
    upper(value);upper(query);return query.empty()||value.find(query)!=std::wstring::npos;
}
bool changedPath(const RenamePreview& row) {return row.proposal.source.native()!=row.target.native();}
bool sameArtist(const Artist& first,const Artist& second) {
    return first.id==second.id&&first.name==second.name&&first.fileName==second.fileName&&first.aliases==second.aliases&&first.order==second.order&&first.fields==second.fields;
}
}

struct ArtistRegistry::Impl {
    HWND owner=nullptr,window=nullptr,viewport=nullptr,content=nullptr;
    UINT notify=0,dpi=96;
    BeforeFilesChange before;
    std::filesystem::path database;
    std::shared_ptr<CollectionStore> store;
    std::jthread worker;
    std::map<int,HWND> controls;
    HFONT font=nullptr,smallFont=nullptr;
    HBRUSH bgBrush=CreateSolidBrush(bg),inputBrush=CreateSolidBrush(input);
    CollectionSnapshot data;
    std::vector<LibraryId> shown;
    LibraryId selected;
    Artist original,planned;
    std::vector<RenamePreview> preview;
    std::optional<RenameResult> fileChanges;
    int tab=0,scrollOffset=0,scrollLimit=0;
    bool filling=false,advanced=false,refreshPending=false,refreshDiscardPending=false,shownOnce=false,layingOut=false,dirty=false;
    struct Pending {
        CollectionSnapshot data;
        LibraryId selected;
        std::vector<RenamePreview> preview;
        bool replacePreview=false;
        Artist planned;
        std::optional<RenameResult> files;
        std::wstring message;
        bool notifyOwner=false;
        bool preserveEditor=false;
    } pending;

    int dp(int value) const {return MulDiv(value,static_cast<int>(dpi),96);}
    HWND c(int id) const {const auto it=controls.find(id);return it==controls.end()?nullptr:it->second;}
    bool busy() const {return worker.joinable();}
    Impl(HWND parent,UINT message,BeforeFilesChange callback,std::filesystem::path path)
        :owner(parent),notify(message),before(std::move(callback)),database(std::move(path)) {
        try {
            const INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};
            if(!InitCommonControlsEx(&common)) throw LibraryError("Cannot initialize artist controls");
            static const ATOM klass=[] {
                WNDCLASSW type{};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;
                type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"Viewery.Artists.1";return RegisterClassW(&type);
            }();
            if(!klass) throw LibraryError("Cannot register artist registry");
            if(owner) dpi=GetDpiForWindow(owner);if(!dpi) dpi=96;
            window=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.Artists.1",L"Художники",
                WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,dp(620),dp(760),owner,nullptr,GetModuleHandleW(nullptr),this);
            if(!window) throw LibraryError("Cannot create artist registry");
            const BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
            viewport=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.Artists.1",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,
                0,0,0,0,window,reinterpret_cast<HMENU>(viewportId),GetModuleHandleW(nullptr),this);
            content=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.Artists.1",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
                0,0,0,0,viewport,reinterpret_cast<HMENU>(contentId),GetModuleHandleW(nullptr),this);
            if(!viewport||!content) throw LibraryError("Cannot create artist registry viewport");
            build();makeFonts();layout();
        } catch(...) {
            if(window) DestroyWindow(window);DeleteObject(font);DeleteObject(smallFont);DeleteObject(bgBrush);DeleteObject(inputBrush);throw;
        }
    }
    ~Impl() {
        worker.request_stop();if(worker.joinable()) worker.join();
        if(window) DestroyWindow(window);DeleteObject(font);DeleteObject(smallFont);DeleteObject(bgBrush);DeleteObject(inputBrush);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self) return DefWindowProcW(hwnd,message,w,l);
        try {return self->dispatch(hwnd,message,w,l);}
        catch(const std::exception& error) {if(self->c(statusLabel)) setText(self->c(statusLabel),wide(error.what()));return 0;}
    }
    static LRESULT CALLBACK inputProc(HWND hwnd,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR value) {
        auto& self=*reinterpret_cast<Impl*>(value);
        if(message==WM_NOTIFY) {
            const auto& notification=*reinterpret_cast<NMHDR*>(l);
            if(notification.code==NM_CUSTOMDRAW&&notification.hwndFrom==ListView_GetHeader(hwnd)) {
                auto& draw=*reinterpret_cast<NMCUSTOMDRAW*>(l);
                if(draw.dwDrawStage==CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW|CDRF_NOTIFYPOSTPAINT;
                if(draw.dwDrawStage==CDDS_ITEMPREPAINT) {
                    fill(draw.hdc,draw.rc,input);const auto old=SelectObject(draw.hdc,self.font);
                    wchar_t caption[128]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=caption;item.cchTextMax=128;
                    Header_GetItem(notification.hwndFrom,static_cast<int>(draw.dwItemSpec),&item);
                    auto area=draw.rc;area.left+=self.dp(6);area.right-=self.dp(6);SetBkMode(draw.hdc,TRANSPARENT);SetTextColor(draw.hdc,fg);
                    DrawTextW(draw.hdc,caption,-1,&area,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                    SelectObject(draw.hdc,old);return CDRF_SKIPDEFAULT;
                }
                if(draw.dwDrawStage==CDDS_POSTPAINT) {
                    RECT empty{};GetClientRect(notification.hwndFrom,&empty);const int count=Header_GetItemCount(notification.hwndFrom);
                    if(count) {RECT last{};Header_GetItemRect(notification.hwndFrom,count-1,&last);empty.left=last.right;}
                    fill(draw.hdc,empty,input);return CDRF_DODEFAULT;
                }
            }
        }
        if(message==WM_KEYDOWN&&w==VK_ESCAPE) {ShowWindow(self.window,SW_HIDE);return 0;}
        if(message==WM_MOUSEWHEEL) {
            wchar_t type[32]{};GetClassNameW(hwnd,type,32);
            if(wcscmp(type,WC_LISTVIEWW)==0) return DefSubclassProc(hwnd,message,w,l);
            return SendMessageW(self.viewport,message,w,l);
        }
        return DefSubclassProc(hwnd,message,w,l);
    }
    HWND create(int id,const wchar_t* type,const wchar_t* caption,DWORD style,bool fixed=false) {
        HWND result=CreateWindowExW(0,type,caption,WS_CHILD|WS_CLIPSIBLINGS|style|(wcscmp(type,L"STATIC")==0?0:WS_TABSTOP),
            0,0,0,0,fixed?window:content,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!result) throw LibraryError("Cannot create artist registry control");
        controls.emplace(id,result);SetWindowTheme(result,L"DarkMode_Explorer",nullptr);
        SetWindowSubclass(result,inputProc,1,reinterpret_cast<DWORD_PTR>(this));
        if(wcscmp(type,WC_LISTVIEWW)==0) {
            ListView_SetBkColor(result,bg);ListView_SetTextBkColor(result,bg);ListView_SetTextColor(result,fg);
            ListView_SetExtendedListViewStyle(result,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
        }
        return result;
    }
    void button(int id,const wchar_t* caption,bool fixed=false) {create(id,L"BUTTON",caption,BS_OWNERDRAW,fixed);}
    void label(int id,const wchar_t* caption) {create(id,L"STATIC",caption,SS_LEFT|SS_NOPREFIX);}
    void edit(int id,DWORD style=0) {create(id,L"EDIT",L"",ES_AUTOHSCROLL|style);}
    void column(int id,int index,const wchar_t* caption,int width) {
        LVCOLUMNW value{};value.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;value.iSubItem=index;
        value.pszText=const_cast<wchar_t*>(caption);value.cx=dp(width);ListView_InsertColumn(c(id),index,&value);
    }
    void build() {
        button(tabRegistry,L"Справочник",true);button(tabFiles,L"Имена работ",true);button(closeButton,L"Закрыть",true);
        create(statusLabel,L"STATIC",L"",SS_LEFT|SS_NOPREFIX,true);create(summaryLabel,L"STATIC",L"Художники",SS_LEFT|SS_NOPREFIX,true);
        label(searchLabel,L"Поиск художника");edit(searchEdit);
        create(artistList,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS);
        column(artistList,0,L"Художник",250);column(artistList,1,L"Работы",75);column(artistList,2,L"Связанные файлы",145);
        button(newButton,L"Добавить");button(deleteButton,L"Удалить из базы…");button(reloadButton,L"Обновить");
        label(nameLabel,L"Имя художника");edit(nameEdit);
        label(fileNameLabel,L"Имя в файлах · пусто — как имя художника");edit(fileNameEdit);
        button(advancedButton,L"Псевдонимы  ▾");label(aliasesLabel,L"Псевдонимы и другие написания");edit(aliasesEdit,ES_MULTILINE|ES_WANTRETURN|WS_VSCROLL);
        button(saveButton,L"Сохранить");button(previewButton,L"Проверить имена связанных файлов…");
        label(previewLabel,L"Проверьте все имена до применения");
        create(previewList,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS);
        column(previewList,0,L"Сейчас",190);column(previewList,1,L"Новое имя",190);column(previewList,2,L"Состояние",130);
        button(applyButton,L"Сохранить и переименовать");button(backButton,L"Назад");button(cancelButton,L"Отменить задачу",true);
    }
    void makeFonts() {
        const HFONT old=font,oldSmall=smallFont;
        font=CreateFontW(-dp(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        smallFont=CreateFontW(-dp(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
        for(const auto& [id,control]:controls) {
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(id==statusLabel||id==previewLabel?smallFont:font),FALSE);
            if(id==nameEdit||id==fileNameEdit||id==searchEdit||id==editName||id==aliasesEdit)
                SendMessageW(control,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(dp(5),dp(5)));
        }
        DeleteObject(old);DeleteObject(oldSmall);
    }
    void put(int id,int x,int y,int width,int height,std::set<int>& visible) {
        visible.insert(id);SetWindowPos(c(id),nullptr,dp(x),dp(y),dp(std::max(1,width)),dp(std::max(1,height)),
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW|SWP_NOREDRAW);
        const bool enabled=!busy()||id==closeButton||id==statusLabel||id==summaryLabel||id==cancelButton;
        EnableWindow(c(id),enabled);
    }
    void layout() {
        if(!viewport||!content||layingOut) return;layingOut=true;
        RECT area{};GetClientRect(window,&area);const int width=MulDiv(area.right,96,static_cast<int>(dpi));
        const int height=MulDiv(area.bottom,96,static_cast<int>(dpi)),bodyHeight=std::max(1,height-102);
        SetWindowPos(viewport,nullptr,0,dp(52),area.right,dp(bodyHeight),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        const int contentHeight=std::max(bodyHeight,tab==0?(advanced?660:570):460);
        scrollLimit=std::max(0,contentHeight-bodyHeight);scrollOffset=std::clamp(scrollOffset,0,scrollLimit);
        SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};scroll.nMax=contentHeight-1;scroll.nPage=static_cast<UINT>(bodyHeight);scroll.nPos=scrollOffset;
        SetScrollInfo(viewport,SB_VERT,&scroll,TRUE);
        RECT body{};GetClientRect(viewport,&body);const int bodyWidth=MulDiv(body.right,96,static_cast<int>(dpi)),inside=std::max(1,bodyWidth-24);
        SetWindowPos(content,nullptr,0,-dp(scrollOffset),body.right,dp(contentHeight),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        std::set<int> visible;const auto p=[&](int id,int x,int y,int w,int h=30){put(id,x,y,w,h,visible);};
        p(tabRegistry,12,10,130,31);p(tabFiles,148,10,130,31);p(summaryLabel,290,15,std::max(1,width-400),23);
        p(closeButton,width-98,height-40,86,30);p(statusLabel,12,height-42,std::max(1,width-(busy()?250:120)),38);
        if(busy()) p(cancelButton,width-234,height-40,130,30);
        if(tab==0) {
            p(searchLabel,12,5,inside,20);p(searchEdit,12,28,inside);
            p(artistList,12,68,inside,180);p(newButton,12,258,100);p(deleteButton,120,258,inside-228);p(reloadButton,bodyWidth-122,258,110);
            p(nameLabel,12,302,inside,20);p(nameEdit,12,325,inside);
            p(fileNameLabel,12,366,inside,20);p(fileNameEdit,12,389,inside);
            p(advancedButton,12,430,inside);int y=472;
            if(advanced) {p(aliasesLabel,12,y,inside,20);p(aliasesEdit,12,y+23,inside,60);y+=96;}
            p(saveButton,12,y,inside,34);p(previewButton,12,y+45,inside,34);
        } else {
            p(previewLabel,12,7,inside,42);const int listHeight=std::max(170,contentHeight-112);
            p(previewList,12,55,inside,listHeight);
            p(backButton,12,67+listHeight,90,34);p(applyButton,110,67+listHeight,inside-98,34);
        }
        for(const auto& [id,control]:controls) if(!visible.contains(id)) ShowWindow(control,SW_HIDE);
        const int usable=std::max(dp(300),dp(inside)-GetSystemMetrics(SM_CXVSCROLL)-3);
        ListView_SetColumnWidth(c(artistList),0,usable*52/100);ListView_SetColumnWidth(c(artistList),1,usable*16/100);ListView_SetColumnWidth(c(artistList),2,usable*32/100);
        ListView_SetColumnWidth(c(previewList),0,usable*36/100);ListView_SetColumnWidth(c(previewList),1,usable*40/100);ListView_SetColumnWidth(c(previewList),2,usable*24/100);
        EnableWindow(c(deleteButton),!busy()&&!selected.empty());EnableWindow(c(previewButton),!busy()&&!selected.empty());
        EnableWindow(c(applyButton),!busy()&&!preview.empty()&&std::ranges::all_of(preview,[](const RenamePreview& row){return row.ready;}));
        layingOut=false;RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    void scrollTo(int value) {
        const int next=std::clamp(value,0,scrollLimit);if(next==scrollOffset) return;
        scrollOffset=next;SetScrollPos(viewport,SB_VERT,scrollOffset,TRUE);
        SetWindowPos(content,nullptr,0,-dp(scrollOffset),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        RedrawWindow(viewport,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    template<class Task> bool start(Task task,const std::wstring& message,bool notifyOwner=false,bool preserveEditor=false) {
        if(busy()) return false;setText(c(statusLabel),message);pending={};pending.selected=selected;pending.notifyOwner=notifyOwner;pending.preserveEditor=preserveEditor;
        worker=std::jthread([this,task=std::move(task)](std::stop_token stop) {
            try {
                if(!store) store=std::make_shared<CollectionStore>(database.empty()?defaultDatabasePath():database);
                task(*store,stop);pending.data=store->snapshot();
                if(pending.message.empty()) pending.message=L"Готово";
            } catch(const std::exception& error) {
                pending.message=wide(error.what());
                if(store) {try {pending.data=store->snapshot();}catch(...) {}}
            }
            PostMessageW(window,completed,0,0);
        });
        layout();return true;
    }
    void refresh(bool discard=false) {
        if(busy()) {refreshPending=true;refreshDiscardPending=refreshDiscardPending||discard;return;}
        start([](CollectionStore&,std::stop_token){},L"Загрузка художников…",false,!discard&&(dirty||!preview.empty()));
    }
    void finish() {
        worker.join();bool stale=false;
        if(pending.preserveEditor&&!original.id.empty()) {
            const auto current=std::ranges::find(pending.data.artists,original.id,&Artist::id);
            stale=current==pending.data.artists.end()||!sameArtist(*current,original);
        }
        data=std::move(pending.data);selected=std::move(pending.selected);
        if(pending.replacePreview) {preview=std::move(pending.preview);planned=std::move(pending.planned);tab=1;scrollOffset=0;}
        if(pending.files) {fileChanges=std::move(pending.files);tab=0;scrollOffset=0;}
        setText(c(statusLabel),pending.message);fillArtists();
        if(!pending.replacePreview&&!pending.preserveEditor) {fillEditor();preview.clear();}
        else if(pending.preserveEditor&&stale) {
            preview.clear();tab=0;scrollOffset=0;
            setText(c(statusLabel),L"Художник изменён извне. Черновик сохранён; «Обновить» загрузит актуальные поля.");
        } else if(pending.preserveEditor&&dirty) setText(c(statusLabel),L"Данные обновлены. Изменения в полях не сохранены.");
        fillPreview();layout();if(pending.notifyOwner&&owner&&notify)PostMessageW(owner,notify,0,0);
        if(std::exchange(refreshPending,false)) refresh(std::exchange(refreshDiscardPending,false));
    }
    std::pair<std::size_t,std::size_t> counts(const LibraryId& id) const {
        std::set<LibraryId> works;for(const auto& work:data.artworks) if(std::ranges::find(work.artists,id)!=work.artists.end()) works.insert(work.id);
        const auto files=std::ranges::count_if(data.files,[&](const MediaRecord& file){return works.contains(file.artworkId);});
        return {works.size(),static_cast<std::size_t>(files)};
    }
    void fillArtists() {
        filling=true;shown.clear();ListView_DeleteAllItems(c(artistList));const auto query=text(c(searchEdit));
        std::vector<const Artist*> ordered;for(const auto& artist:data.artists) if(contains(artist.name,query)||contains(artist.aliases,query)||contains(artist.fileName,query)) ordered.push_back(&artist);
        std::ranges::sort(ordered,[](const Artist* a,const Artist* b){return CompareStringOrdinal(a->name.c_str(),-1,b->name.c_str(),-1,TRUE)==CSTR_LESS_THAN;});
        for(const auto* artist:ordered) {
            const int row=static_cast<int>(shown.size());shown.push_back(artist->id);
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=const_cast<wchar_t*>(artist->name.c_str());ListView_InsertItem(c(artistList),&item);
            const auto [works,files]=counts(artist->id);auto workText=std::to_wstring(works),fileText=std::to_wstring(files);
            ListView_SetItemText(c(artistList),row,1,workText.data());ListView_SetItemText(c(artistList),row,2,fileText.data());
            if(artist->id==selected) ListView_SetItemState(c(artistList),row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        }
        setText(c(summaryLabel),L"Всего: "+std::to_wstring(data.artists.size()));filling=false;
    }
    void fillEditor() {
        filling=true;original={};
        const auto found=std::ranges::find(data.artists,selected,&Artist::id);
        if(found!=data.artists.end()) original=*found;else selected.clear();
        setText(c(nameEdit),original.name);setText(c(fileNameEdit),original.fileName==original.name?L"":original.fileName);setText(c(aliasesEdit),original.aliases);
        filling=false;dirty=false;
    }
    Artist edited() const {
        auto artist=original;artist.id=selected;artist.name=trim(text(c(nameEdit)));artist.fileName=trim(text(c(fileNameEdit)));artist.aliases=trim(text(c(aliasesEdit)));
        if(artist.name.empty()) throw LibraryError("Введите имя художника");if(artist.fileName.empty())artist.fileName=artist.name;return artist;
    }
    void newArtist() {selected.clear();preview.clear();tab=0;scrollOffset=0;fillArtists();fillEditor();fillPreview();layout();SetFocus(c(nameEdit));}
    void makePreview() {
        const auto value=edited();if(value.id.empty()) throw LibraryError("Сначала сохраните нового художника");
        start([this,value](CollectionStore& db,std::stop_token) {
            pending.preview=db.previewArtistRename(value);pending.planned=value;pending.replacePreview=true;
            const auto changed=std::ranges::count_if(pending.preview,changedPath);
            pending.message=L"Изменится имён: "+std::to_wstring(changed)+L". Без изменения имени: "+std::to_wstring(pending.preview.size()-static_cast<std::size_t>(changed));
        },L"Проверка связанных файлов…");
    }
    void save() {
        const auto value=edited();const auto [works,files]=counts(value.id);(void)works;
        if(!value.id.empty()&&(value.name!=original.name||value.fileName!=original.fileName)) {
            if(files) {makePreview();return;}
            const auto expected=original;
            start([this,expected,value](CollectionStore& db,std::stop_token stop) {
                pending.files=db.renameArtist(expected,value,{},stop);pending.message=pending.files->message;
            },L"Сохранение художника…",true);return;
        }
        start([this,value](CollectionStore& db,std::stop_token) {pending.selected=db.saveArtist(value);pending.message=L"Художник сохранён";},L"Сохранение художника…",true);
    }
    void fillPreview() {
        filling=true;ListView_DeleteAllItems(c(previewList));std::size_t changed=0,problems=0;
        for(std::size_t index=0;index<preview.size();++index) {
            const auto& row=preview[index];LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(index);
            auto old=row.proposal.source.filename().wstring();item.pszText=old.data();ListView_InsertItem(c(previewList),&item);
            ListView_SetItemText(c(previewList),static_cast<int>(index),1,const_cast<wchar_t*>(row.proposal.newName.c_str()));
            const bool changes=changedPath(row);if(changes) ++changed;if(!row.ready) ++problems;
            const auto state=row.ready?(changes?L"Готов":L"Без изменений"):row.problem.c_str();
            ListView_SetItemText(c(previewList),static_cast<int>(index),2,const_cast<wchar_t*>(state));
        }
        setText(c(previewLabel),planned.name+L" · связанных файлов: "+std::to_wstring(preview.size())+L"\nНовых имён: "+std::to_wstring(changed)+L" · конфликтов: "+std::to_wstring(problems));
        filling=false;
    }
    void apply() {
        if(preview.empty()||!std::ranges::all_of(preview,[](const RenamePreview& row){return row.ready;})) throw LibraryError("Устраните конфликты перед переименованием");
        const auto changed=std::ranges::count_if(preview,changedPath);
        const auto message=L"Сохранить художника «"+planned.name+L"» и обновить имена файлов: "+std::to_wstring(changed)+L"?\nОстальные связанные файлы сохранят имена. Содержимое файлов не изменится.";
        if(MessageBoxW(window,message.c_str(),L"Художник и работы",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        std::vector<RenameProposal> proposals;for(const auto& row:preview) proposals.push_back(row.proposal);
        if(changed&&before) before();const auto value=planned;const auto expected=original;
        start([this,expected,value,proposals](CollectionStore& db,std::stop_token stop) {
            pending.files=db.renameArtist(expected,value,proposals,stop);pending.message=pending.files->message;
        },L"Переименование художника и работ…",true);
    }
    void remove() {
        if(selected.empty()) return;
        const auto [works,files]=counts(selected);
        const auto message=L"Удалить художника «"+original.name+L"» из базы?\nСвязанных работ: "+std::to_wstring(works)+L", файлов: "+std::to_wstring(files)+L".\nФайлы на диске и другие художники не удаляются.";
        if(MessageBoxW(window,message.c_str(),L"Удаление художника",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return;
        const auto id=selected;
        start([this,id](CollectionStore& db,std::stop_token) {db.deleteArtist(id);pending.selected.clear();pending.message=L"Художник удалён из базы. Файлы сохранены.";},L"Удаление художника…",true);
    }
    void command(int id,int notification) {
        if(filling) return;
        if(id==closeButton) {ShowWindow(window,SW_HIDE);return;}
        if(id==cancelButton) {worker.request_stop();return;}
        if(busy()) return;
        if(id==searchEdit&&notification==EN_CHANGE) {fillArtists();return;}
        if((id==nameEdit||id==fileNameEdit||id==aliasesEdit)&&notification==EN_CHANGE) {dirty=true;preview.clear();fillPreview();setText(c(statusLabel),L"Изменения не сохранены");return;}
        if(notification!=BN_CLICKED) return;
        switch(id) {
        case tabRegistry:tab=0;scrollOffset=0;layout();break;
        case tabFiles:if(preview.empty()) makePreview();else {tab=1;scrollOffset=0;layout();}break;
        case newButton:newArtist();break;
        case reloadButton:refresh(true);break;
        case deleteButton:remove();break;
        case advancedButton:advanced=!advanced;setText(c(advancedButton),advanced?L"Псевдонимы  ▴":L"Псевдонимы  ▾");layout();break;
        case saveButton:save();break;
        case previewButton:makePreview();break;
        case applyButton:apply();break;
        case backButton:tab=0;scrollOffset=0;layout();break;
        default:break;
        }
    }
    LRESULT dispatch(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        switch(message) {
        case completed:finish();return 0;
        case WM_SIZE:if(hwnd==window) layout();return 0;
        case WM_DPICHANGED: {
            if(hwnd!=window) break;dpi=HIWORD(w);makeFonts();const auto bounds=*reinterpret_cast<RECT*>(l);
            SetWindowPos(window,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;
        }
        case WM_GETMINMAXINFO: {
            if(hwnd!=window) break;auto& limits=*reinterpret_cast<MINMAXINFO*>(l);limits.ptMinTrackSize={dp(500),dp(500)};return 0;
        }
        case WM_CLOSE:ShowWindow(window,SW_HIDE);return 0;
        case WM_KEYDOWN:if(w==VK_ESCAPE) {ShowWindow(window,SW_HIDE);return 0;}break;
        case WM_COMMAND:command(LOWORD(w),HIWORD(w));return 0;
        case WM_NOTIFY: {
            const auto* header=reinterpret_cast<NMHDR*>(l);
            if(filling||busy()) return 0;
            if(header->idFrom==artistList&&header->code==LVN_ITEMCHANGED) {
                const auto& change=*reinterpret_cast<NMLISTVIEW*>(l);
                if((change.uNewState&LVIS_SELECTED)&&!(change.uOldState&LVIS_SELECTED)&&change.iItem>=0&&change.iItem<static_cast<int>(shown.size())) {
                    selected=shown[static_cast<std::size_t>(change.iItem)];preview.clear();fillEditor();fillPreview();layout();
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL:scrollTo(scrollOffset-MulDiv(GET_WHEEL_DELTA_WPARAM(w),45,WHEEL_DELTA));return 0;
        case WM_VSCROLL: {
            int next=scrollOffset;SCROLLINFO value{sizeof(value),SIF_TRACKPOS};GetScrollInfo(viewport,SB_VERT,&value);
            switch(LOWORD(w)) {case SB_LINEUP:next-=32;break;case SB_LINEDOWN:next+=32;break;case SB_PAGEUP:next-=180;break;case SB_PAGEDOWN:next+=180;break;case SB_TOP:next=0;break;case SB_BOTTOM:next=scrollLimit;break;case SB_THUMBTRACK:case SB_THUMBPOSITION:next=value.nTrackPos;break;default:break;}
            scrollTo(next);return 0;
        }
        case WM_ERASEBKGND: {RECT area{};GetClientRect(hwnd,&area);fill(reinterpret_cast<HDC>(w),area,bg);return 1;}
        case WM_PAINT: {PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);fill(dc,paint.rcPaint,bg);EndPaint(hwnd,&paint);return 0;}
        case WM_CTLCOLOREDIT:case WM_CTLCOLORSTATIC:case WM_CTLCOLORLISTBOX: {
            HDC dc=reinterpret_cast<HDC>(w);SetTextColor(dc,fg);SetBkColor(dc,message==WM_CTLCOLORSTATIC?bg:input);return reinterpret_cast<LRESULT>(message==WM_CTLCOLORSTATIC?bgBrush:inputBrush);
        }
        case WM_DRAWITEM: {
            auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(l);const bool active=item.CtlID==static_cast<UINT>(tabRegistry+tab);
            fill(item.hDC,item.rcItem,active?RGB(42,37,69):input);const auto old=SelectObject(item.hDC,font);
            SetTextColor(item.hDC,(item.itemState&ODS_DISABLED)?muted:active?accent:fg);SetBkMode(item.hDC,TRANSPARENT);
            RECT bounds=item.rcItem;bounds.left+=dp(5);bounds.right-=dp(5);const auto value=text(item.hwndItem);
            DrawTextW(item.hDC,value.c_str(),static_cast<int>(value.size()),&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            if(item.itemState&ODS_FOCUS) {InflateRect(&bounds,-dp(2),-dp(3));DrawFocusRect(item.hDC,&bounds);}
            SelectObject(item.hDC,old);return TRUE;
        }
        default:break;
        }
        return DefWindowProcW(hwnd,message,w,l);
    }
};

ArtistRegistry::ArtistRegistry(HWND owner,UINT changedMessage,BeforeFilesChange beforeChange,std::filesystem::path database)
    :impl_(std::make_unique<Impl>(owner,changedMessage,std::move(beforeChange),std::move(database))) {}
ArtistRegistry::~ArtistRegistry()=default;
void ArtistRegistry::show(UINT dpi) {
    if(dpi&&dpi!=impl_->dpi) {impl_->dpi=dpi;impl_->makeFonts();impl_->layout();}
    if(!impl_->shownOnce) {impl_->shownOnce=true;impl_->refresh();}
    ShowWindow(impl_->window,SW_SHOWNORMAL);SetForegroundWindow(impl_->window);
}
void ArtistRegistry::hide() {ShowWindow(impl_->window,SW_HIDE);}
HWND ArtistRegistry::window() const {return impl_->window;}
bool ArtistRegistry::processMessage(MSG& message) {
    if(!IsWindowVisible(impl_->window)||(message.hwnd!=impl_->window&&!IsChild(impl_->window,message.hwnd))) return false;
    if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE) {hide();return true;}
    if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN) {
        wchar_t type[32]{};GetClassNameW(message.hwnd,type,32);
        if(wcscmp(type,L"Button")==0) {SendMessageW(message.hwnd,BM_CLICK,0,0);return true;}
    }
    return message.message==WM_KEYDOWN&&message.wParam==VK_TAB&&IsDialogMessageW(impl_->window,&message);
}
bool ArtistRegistry::busy() const {return impl_->busy();}
void ArtistRegistry::cancel() {impl_->worker.request_stop();}
void ArtistRegistry::refresh() {impl_->refresh();}
std::optional<RenameResult> ArtistRegistry::takeFileChanges() {return std::exchange(impl_->fileChanges,std::nullopt);}

} // namespace viewery
