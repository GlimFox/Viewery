#include "rename_studio.hpp"
#include "rename_template.hpp"

#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace viewery {
namespace {
namespace fs = std::filesystem;
constexpr UINT finished = WM_APP + 431;
constexpr COLORREF bg = RGB(23,26,33), fg = RGB(241,242,247), muted = RGB(149,155,170);
constexpr COLORREF accent = RGB(155,140,255), inputColor = RGB(36,40,52);
enum Id {
    tabAssign=1000, tabTemplate, tabPreview, tabHistory, summary, status,
    artist, title, work, assignment, workNumber, versionNumber, assign, fields, aliases,
    preset, presetSave, presetDuplicate, presetDelete, presetImport, presetExport,
    blocks, blockKind, blockAdd, blockRemove, blockUp, blockDown, blockText, blockDigits,
    blockPrefix, blockSuffix, blockFallback, blockCase, blockApply, authorJoin,
    counterStart, counterScope, previewList, previewSearch, newName, editName, makePreview,
    apply, historyList, undo, recover, cancelTask, dbBackup, metaExport, metaImport, historyDetails
};
std::wstring text(HWND control) {
    std::wstring result(static_cast<std::size_t>(GetWindowTextLengthW(control))+1,L'\0');
    const auto length=GetWindowTextW(control,result.data(),static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}
std::wstring wide(std::string_view input) {
    const int size=MultiByteToWideChar(CP_UTF8,0,input.data(),static_cast<int>(input.size()),nullptr,0);
    std::wstring output(static_cast<std::size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,0,input.data(),static_cast<int>(input.size()),output.data(),size);
    return output;
}
std::string utf8(std::wstring_view input) {
    const int size=WideCharToMultiByte(CP_UTF8,0,input.data(),static_cast<int>(input.size()),nullptr,0,nullptr,nullptr);
    std::string output(static_cast<std::size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,0,input.data(),static_cast<int>(input.size()),output.data(),size,nullptr,nullptr);
    return output;
}
std::int64_t integer(HWND control,std::int64_t fallback=1) {
    const auto value=text(control);
    if(value.empty()) return fallback;
    wchar_t* end=nullptr;
    const auto number=wcstoll(value.c_str(),&end,10);
    if(end!=value.c_str()+value.size()||number<0||number>999999999) throw LibraryError("Invalid number");
    return number;
}
std::wstring trim(std::wstring value) {
    const auto first=value.find_first_not_of(L" \r\n\t");
    if(first==std::wstring::npos) return {};
    return value.substr(first,value.find_last_not_of(L" \r\n\t")-first+1);
}
std::vector<std::wstring> split(std::wstring_view value,wchar_t delimiter) {
    std::vector<std::wstring> parts;
    std::wistringstream input{std::wstring(value)};
    std::wstring part;
    while(std::getline(input,part,delimiter)) if(auto clean=trim(part);!clean.empty()) parts.push_back(std::move(clean));
    return parts;
}
Metadata parseFields(const std::wstring& value) {
    Metadata result;
    for(const auto& entry:split(value,L'\n')) {
        const auto equal=entry.find(L'=');
        if(equal==std::wstring::npos||equal==0) throw LibraryError("Use key=value for each custom field");
        const auto key=trim(entry.substr(0,equal));
        if(key.empty()) throw LibraryError("Custom field names cannot be empty");
        if(!result.emplace(key,trim(entry.substr(equal+1))).second) {
            throw LibraryError("A custom field name occurs more than once");
        }
    }
    return result;
}
std::optional<fs::path> pick(HWND owner,bool save,std::wstring name,bool database=false) {
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    if(FAILED(CoCreateInstance(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return {};
    const COMDLG_FILTERSPEC type{database?L"SQLite":L"JSON",database?L"*.sqlite3":L"*.json"};
    dialog->SetFileTypes(1,&type);
    dialog->SetDefaultExtension(database?L"sqlite3":L"json");
    if(!name.empty()) dialog->SetFileName(name.c_str());
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR|FOS_OVERWRITEPROMPT);
    if(FAILED(dialog->Show(owner))) return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path=nullptr;
    if(FAILED(dialog->GetResult(&item))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) return {};
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}
void fill(HDC dc,RECT rect,COLORREF color) {
    HBRUSH brush=CreateSolidBrush(color);
    FillRect(dc,&rect,brush);
    DeleteObject(brush);
}
}

struct RenameStudio::Impl {
    HWND owner=nullptr,window=nullptr;
    UINT changed=0,dpi=96;
    HFONT font=nullptr;
    HBRUSH brush=CreateSolidBrush(bg),editBrush=CreateSolidBrush(inputColor);
    std::map<int,HWND> controls;
    std::vector<HWND> labels;
    int tab=0,dragBlock=-1,scrollOffset=0,scrollLimit=0;
    bool filling=false,indexPending=false;
    std::uint64_t selectionGeneration=0,directoryGeneration=0;
    std::vector<fs::path> directory,selection,indexed;
    CollectionSnapshot data;
    std::vector<LibraryPreset> presets;
    RenameTemplate naming=defaultArtistTemplate();
    LibraryId selectedWorkId;
    LibraryId selectedOperationId;
    std::vector<RenamePreview> preview;
    std::vector<std::size_t> shown;
    std::vector<RenameOperation> operations;
    std::shared_ptr<CollectionStore> store;
    fs::path database;
    std::jthread worker;
    BeforeFilesChange before;
    std::optional<RenameResult> fileResult;
    std::wstring resultMessage;
    int dp(int value) const { return MulDiv(value,static_cast<int>(dpi),96); }
    HWND c(int id) const { const auto found=controls.find(id);return found==controls.end()?nullptr:found->second; }
    bool busy() const { return worker.joinable(); }

    Impl(HWND parent,UINT msg,BeforeFilesChange callback,fs::path databasePath)
        :owner(parent),changed(msg),database(std::move(databasePath)),before(std::move(callback)) {
        static const ATOM klass=[] {
            WNDCLASSW wc{};
            wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpfnWndProc=procedure;
            wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            wc.lpszClassName=L"Viewery.RenameStudio.1";
            return RegisterClassW(&wc);
        }();
        if(!klass) throw LibraryError("Cannot register Rename Studio");
        window=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.RenameStudio.1",L"Rename Studio",WS_CHILD|WS_CLIPCHILDREN|WS_VSCROLL,
                               0,0,0,0,parent,nullptr,GetModuleHandleW(nullptr),this);
        if(!window) throw LibraryError("Cannot create Rename Studio");
        build();
    }
    ~Impl() {
        worker.request_stop();
        if(worker.joinable()) worker.join();
        if(window) DestroyWindow(window);
        DeleteObject(brush);
        DeleteObject(editBrush);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        return self?self->dispatch(hwnd,msg,w,l):DefWindowProcW(hwnd,msg,w,l);
    }
    HWND create(int id,const wchar_t* type,const wchar_t* value,DWORD style) {
        HWND result=CreateWindowExW(0,type,value,WS_CHILD|WS_TABSTOP|style,0,0,0,0,window,
                                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        controls[id]=result;
        if(wcscmp(type,WC_LISTVIEWW)==0) {
            ListView_SetBkColor(result,bg);
            ListView_SetTextBkColor(result,bg);
            ListView_SetTextColor(result,fg);
            ListView_SetExtendedListViewStyle(result,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        }
        SetWindowTheme(result,L"DarkMode_Explorer",nullptr);
        return result;
    }
    void button(int id,const wchar_t* value) { create(id,L"BUTTON",value,BS_OWNERDRAW); }
    void edit(int id,const wchar_t* value=L"",DWORD extra=0) { create(id,L"EDIT",value,ES_AUTOHSCROLL|extra); }
    void combo(int id,bool editable=false) {
        create(id,L"COMBOBOX",L"",CBS_HASSTRINGS|WS_VSCROLL|(editable?CBS_DROPDOWN:CBS_DROPDOWNLIST));
    }
    void item(int id,const wchar_t* value) { SendMessageW(c(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value)); }
    void column(int id,int index,const wchar_t* name,int width) {
        LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
        col.pszText=const_cast<wchar_t*>(name);col.cx=dp(width);col.iSubItem=index;
        ListView_InsertColumn(c(id),index,&col);
    }
    void build() {
        button(tabAssign,L"Метаданные");button(tabTemplate,L"Шаблон");
        button(tabPreview,L"Предпросмотр");button(tabHistory,L"История");
        create(summary,L"STATIC",L"Выбрано 0 файлов",0);
        create(status,L"STATIC",L"",SS_LEFT);
        combo(artist,true);combo(work,true);edit(title);
        edit(workNumber);edit(versionNumber,L"1");
        combo(assignment);item(assignment,L"Отдельная работа для каждого файла");
        item(assignment,L"Версии одной новой работы");item(assignment,L"Привязать к выбранной работе");
        item(assignment,L"Назначить художников без изменения связей");
        SendMessageW(c(assignment),CB_SETCURSEL,0,0);
        edit(aliases);edit(fields,L"",ES_MULTILINE|ES_WANTRETURN|WS_VSCROLL);
        button(assign,L"Назначить выбранным");
        button(dbBackup,L"Копия базы");button(metaExport,L"Экспорт JSON");button(metaImport,L"Импорт JSON");
        combo(preset,true);button(presetSave,L"Сохранить");button(presetDuplicate,L"Копия");
        button(presetDelete,L"Удалить");button(presetImport,L"Импорт");button(presetExport,L"Экспорт");
        create(blocks,L"LISTBOX",L"",LBS_NOTIFY|WS_VSCROLL|LBS_NOINTEGRALHEIGHT);
        combo(blockKind);
        for(int k=0;k<=static_cast<int>(NameBlockKind::separator);++k) {
            const auto name=std::wstring(nameBlockLabel(static_cast<NameBlockKind>(k)));
            item(blockKind,name.c_str());
        }
        SendMessageW(c(blockKind),CB_SETCURSEL,0,0);
        button(blockAdd,L"Добавить");button(blockRemove,L"Убрать");button(blockUp,L"↑");button(blockDown,L"↓");
        edit(blockText);edit(blockDigits,L"0");edit(blockPrefix);edit(blockSuffix);edit(blockFallback);
        combo(blockCase);item(blockCase,L"Как задано");item(blockCase,L"строчные");item(blockCase,L"ПРОПИСНЫЕ");
        SendMessageW(c(blockCase),CB_SETCURSEL,0,0);
        button(blockApply,L"Обновить блок");
        edit(authorJoin,L" & ");edit(counterStart,L"1");combo(counterScope);
        item(counterScope,L"Общий счётчик");item(counterScope,L"По художнику");item(counterScope,L"По работе");
        SendMessageW(c(counterScope),CB_SETCURSEL,0,0);
        create(previewList,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SHOWSELALWAYS);
        ListView_SetExtendedListViewStyle(c(previewList),LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        column(previewList,0,L"Исходное имя",155);column(previewList,1,L"Новое имя",195);column(previewList,2,L"Статус",120);
        edit(previewSearch);edit(newName);button(editName,L"Изменить имя");
        button(makePreview,L"Обновить предпросмотр");button(apply,L"Переименовать");
        create(historyList,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SHOWSELALWAYS);
        column(historyList,0,L"Время",150);column(historyList,1,L"Состояние",100);column(historyList,2,L"Файлов",65);
        edit(historyDetails,L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL);
        button(undo,L"Отменить операцию");button(recover,L"Восстановить");button(cancelTask,L"Отменить задачу");
        fillBlocks();fillGlobals();
    }
    void position(int id,int x,int y,int width,int height,std::set<int>& visible) {
        visible.insert(id);
        SetWindowPos(c(id),nullptr,dp(x),dp(y-scrollOffset),dp(width),dp(height),
                     SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        const bool available=!busy()||id==cancelTask||id==summary||id==status;
        EnableWindow(c(id),available);
    }
    void caption(const wchar_t* value,int x,int y,int width) {
        const HWND label=CreateWindowExW(0,L"STATIC",value,WS_CHILD|WS_VISIBLE,dp(x),dp(y-scrollOffset),dp(width),dp(20),
                                         window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        labels.push_back(label);
    }
    void layout() {
        RECT rect{};GetClientRect(window,&rect);
        const int width=MulDiv(rect.right,96,static_cast<int>(dpi));
        const int viewport=MulDiv(rect.bottom,96,static_cast<int>(dpi));
        const int height=tab<=1?std::max(viewport,630):std::max(viewport,430);
        scrollLimit=std::max(0,height-viewport);
        scrollOffset=std::clamp(scrollOffset,0,scrollLimit);
        SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
        scroll.nMin=0;scroll.nMax=height-1;scroll.nPage=static_cast<UINT>(std::max(1,viewport));scroll.nPos=scrollOffset;
        SetScrollInfo(window,SB_VERT,&scroll,TRUE);
        for(HWND label:labels) DestroyWindow(label);
        labels.clear();
        std::set<int> visible;
        const auto put=[&](int id,int x,int y,int w,int h=28){position(id,x,y,w,h,visible);};
        const int inside=width-24;
        const int tabWidth=(inside-9)/4;
        for(int index=0;index<4;++index) put(tabAssign+index,12+index*(tabWidth+3),10,tabWidth,29);
        put(summary,12,46,inside,22);
        put(status,12,height-49,inside,40);
        if(tab==0) {
            caption(L"Художники (соавторы через ;)",12,75,inside);
            put(artist,12,96,inside,220);
            caption(L"Псевдонимы художника",12,130,inside);put(aliases,12,151,inside);
            caption(L"Работа (для существующей выберите из списка)",12,185,inside);put(work,12,206,inside,200);
            caption(L"Название работы",12,240,inside);put(title,12,261,inside);
            put(assignment,12,298,inside,140);
            caption(L"№ работы (пусто — следующий)",12,333,inside/2);
            caption(L"Первая версия",width/2+4,333,inside/2-4);
            put(workNumber,12,354,inside/2-6);put(versionNumber,width/2+4,354,inside/2-4);
            caption(L"Поля файлов · ключ=значение",12,388,inside);put(fields,12,409,inside,65);
            put(assign,12,482,inside,32);
            const int third=(inside-8)/3;
            put(dbBackup,12,523,third);put(metaExport,16+third,523,third);put(metaImport,20+third*2,523,third);
        } else if(tab==1) {
            put(preset,12,75,inside,170);
            const int fifth=(inside-12)/5;
            int x=12;
            for(int id:{presetSave,presetDuplicate,presetDelete,presetImport,presetExport}) {
                put(id,x,110,fifth);x+=fifth+3;
            }
            put(blocks,12,147,inside-42,135);
            put(blockUp,width-45,147,32);put(blockDown,width-45,181,32);put(blockRemove,width-83,251,70);
            put(blockKind,12,290,inside-104,200);put(blockAdd,width-107,290,95);
            caption(L"Текст / имя поля / формат даты",12,324,inside);put(blockText,12,344,inside);
            caption(L"Цифры",12,379,70);put(blockDigits,12,399,60);
            caption(L"Префикс",82,379,100);put(blockPrefix,82,399,96);
            caption(L"Суффикс",188,379,100);put(blockSuffix,188,399,inside-176);
            caption(L"Запасное значение",12,433,inside/2);put(blockFallback,12,453,inside/2-4);
            put(blockCase,16+inside/2,453,inside/2-4,130);
            put(blockApply,12,490,inside);
            caption(L"Соавторы",12,524,100);put(authorJoin,12,544,90);
            caption(L"Счётчик с",112,524,90);put(counterStart,112,544,70);
            put(counterScope,192,544,inside-180,130);
        } else if(tab==2) {
            caption(L"Поиск по исходному или новому имени",12,75,inside);
            put(previewSearch,12,96,inside);
            put(previewList,12,132,inside,std::max(125,height-320));
            const int y=height-178;
            put(newName,12,y,inside);put(editName,12,y+34,inside);
            put(makePreview,12,y+68,inside/2-4);put(apply,16+inside/2,y+68,inside/2-4,31);
            put(cancelTask,12,y+103,inside);
        } else {
            const int listHeight=std::max(140,height-355);
            put(historyList,12,80,inside,listHeight);
            put(historyDetails,12,88+listHeight,inside,112);
            put(undo,12,height-146,inside/2-4);put(recover,16+inside/2,height-146,inside/2-4);
            put(cancelTask,12,height-110,inside);
        }
        for(const auto& [id,control]:controls) if(!visible.contains(id)) ShowWindow(control,SW_HIDE);
        InvalidateRect(window,nullptr,FALSE);
    }
    struct JobResult {
        CollectionSnapshot snapshot;
        std::vector<LibraryPreset> presets;
        std::vector<RenameOperation> history;
        std::vector<RenamePreview> preview;
        std::optional<RenameResult> files;
        std::wstring message;
        LibraryId savedPresetId;
        std::optional<std::wstring> operationDetails;
        std::optional<std::pair<std::uint64_t,std::uint64_t>> previewGeneration;
    } pending;
    bool startJob(std::function<void(CollectionStore&,std::stop_token)> task,std::wstring message) {
        if(busy()) return false;
        SetWindowTextW(c(status),message.c_str());
        pending={};
        worker=std::jthread([this,task=std::move(task)](std::stop_token stop) {
            try {
                if(!store) store=std::make_shared<CollectionStore>(database.empty()?defaultDatabasePath():database);
                task(*store,stop);
                pending.snapshot=store->snapshot();
                pending.presets=store->presets();
                pending.history=store->history();
                if(pending.message.empty()) pending.message=L"Готово";
            } catch(const std::exception& e) {
                pending.message=wide(e.what());
                if(store) {
                    try {
                        pending.snapshot=store->snapshot();
                        pending.presets=store->presets();
                        pending.history=store->history();
                    } catch(...) {}
                }
            }
            PostMessageW(window,finished,0,0);
        });
        layout();
        return true;
    }
    void beginIndex() {
        if(busy()) { indexPending=true;return; }
        const auto files=directory;
        indexPending=false;
        startJob([this,files](CollectionStore& db,std::stop_token stop) {
            db.indexFiles(files,stop);
            indexed=files;
        },L"Индексация метаданных…");
    }
    std::vector<MediaRecord> selectedRecords() const {
        std::set<fs::path> paths(selection.begin(),selection.end());
        std::vector<MediaRecord> result;
        for(const auto& record:data.files) if(paths.contains(record.path)) result.push_back(record);
        return result;
    }
    void finishJob() {
        const bool cancelled=worker.get_stop_token().stop_requested();
        worker.join();
        data=std::move(pending.snapshot);
        presets=std::move(pending.presets);
        operations=std::move(pending.history);
        if(pending.previewGeneration&&pending.previewGeneration->first==selectionGeneration&&
            pending.previewGeneration->second==directoryGeneration) preview=std::move(pending.preview);
        if(!pending.savedPresetId.empty()) naming.id=std::move(pending.savedPresetId);
        if(pending.operationDetails) SetWindowTextW(c(historyDetails),pending.operationDetails->c_str());
        if(pending.files) fileResult=std::move(pending.files);
        SetWindowTextW(c(status),pending.message.c_str());
        fillMetadata();
        fillPresets();
        fillPreview();
        fillHistory();
        PostMessageW(owner,changed,0,0);
        layout();
        if(!cancelled&&(indexPending||directory!=indexed)) beginIndex();
    }
    void updateSelection(std::vector<fs::path> selected) {
        ++selectionGeneration;
        selection=std::move(selected);
        const auto value=L"Выбрано файлов   "+std::to_wstring(selection.size());
        SetWindowTextW(c(summary),value.c_str());
        preview.clear();fillPreview();
    }
    void fillMetadata() {
        filling=true;
        const auto artistValue=text(c(artist)),workValue=text(c(work));
        SendMessageW(c(artist),CB_RESETCONTENT,0,0);
        for(const auto& a:data.artists) item(artist,a.name.c_str());
        SetWindowTextW(c(artist),artistValue.c_str());
        SendMessageW(c(work),CB_RESETCONTENT,0,0);
        for(const auto& a:data.artworks) {
            const auto titleValue=std::to_wstring(a.number)+L" · "+a.title;
            item(work,titleValue.c_str());
        }
        const auto selected=std::ranges::find(data.artworks,selectedWorkId,&Artwork::id);
        if(!selectedWorkId.empty()&&selected!=data.artworks.end()) {
            SendMessageW(c(work),CB_SETCURSEL,static_cast<WPARAM>(selected-data.artworks.begin()),0);
        } else SetWindowTextW(c(work),workValue.c_str());
        filling=false;
    }
    void fillPresets() {
        const auto name=text(c(preset));
        SendMessageW(c(preset),CB_RESETCONTENT,0,0);
        for(const auto& p:presets) item(preset,p.name.c_str());
        if(!name.empty()) SetWindowTextW(c(preset),name.c_str());
        else {
            const auto saved=store?store->setting(L"lastPreset"):std::wstring{};
            const auto found=std::ranges::find(presets,saved,&LibraryPreset::id);
            if(found!=presets.end()) {
                try {
                    naming=parseTemplate(utf8(found->json));
                    naming.id=found->id;
                    SetWindowTextW(c(preset),found->name.c_str());
                } catch(const std::exception&) {
                    naming=defaultArtistTemplate();
                    SetWindowTextW(c(preset),naming.name.c_str());
                    SetWindowTextW(c(status),L"Сохранённый шаблон повреждён. Выбран стандартный шаблон.");
                }
                fillBlocks();fillGlobals();
            } else SetWindowTextW(c(preset),naming.name.c_str());
        }
    }
    void fillBlocks() {
        const auto selected=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
        SendMessageW(c(blocks),LB_RESETCONTENT,0,0);
        for(const auto& block:naming.blocks) {
            auto label=std::wstring(nameBlockLabel(block.kind));
            if(!block.text.empty()) label+=L"  "+block.text;
            if(block.digits>0) label+=L" ["+std::to_wstring(block.digits)+L"]";
            SendMessageW(c(blocks),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
        if(selected>=0&&selected<static_cast<int>(naming.blocks.size())) SendMessageW(c(blocks),LB_SETCURSEL,selected,0);
    }
    void fillGlobals() {
        SetWindowTextW(c(authorJoin),naming.authorSeparator.c_str());
        SetWindowTextW(c(counterStart),std::to_wstring(naming.counterStart).c_str());
        SendMessageW(c(counterScope),CB_SETCURSEL,static_cast<WPARAM>(naming.counterScope),0);
    }
    void selectBlock() {
        const auto index=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
        if(index<0||index>=static_cast<int>(naming.blocks.size())) return;
        const auto& block=naming.blocks[static_cast<std::size_t>(index)];
        SetWindowTextW(c(blockText),block.text.c_str());SetWindowTextW(c(blockDigits),std::to_wstring(block.digits).c_str());
        SetWindowTextW(c(blockPrefix),block.prefix.c_str());SetWindowTextW(c(blockSuffix),block.suffix.c_str());
        SetWindowTextW(c(blockFallback),block.fallback.c_str());
        SendMessageW(c(blockCase),CB_SETCURSEL,static_cast<WPARAM>(block.letterCase),0);
    }
    void applyBlock() {
        const auto index=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
        if(index<0||index>=static_cast<int>(naming.blocks.size())) return;
        auto block=naming.blocks[static_cast<std::size_t>(index)];
        block.text=text(c(blockText));block.digits=static_cast<int>(integer(c(blockDigits),0));
        if(block.digits>12) throw LibraryError("Use at most 12 digits");
        block.prefix=text(c(blockPrefix));block.suffix=text(c(blockSuffix));block.fallback=text(c(blockFallback));
        block.letterCase=static_cast<NameCase>(SendMessageW(c(blockCase),CB_GETCURSEL,0,0));
        naming.blocks[static_cast<std::size_t>(index)]=std::move(block);
        fillBlocks();
    }
    void assignMetadata() {
        const auto selected=selectedRecords();
        if(selected.empty()) throw LibraryError("Select files in the catalog first");
        const auto names=split(text(c(artist)),L';');
        const auto extra=parseFields(text(c(fields)));
        const auto titleValue=trim(text(c(title)));
        const auto alias=text(c(aliases));
        const auto mode=SendMessageW(c(assignment),CB_GETCURSEL,0,0);
        const auto chosen=SendMessageW(c(work),CB_GETCURSEL,0,0);
        const LibraryId chosenId=chosen>=0&&chosen<static_cast<LRESULT>(data.artworks.size())
            ?data.artworks[static_cast<std::size_t>(chosen)].id:LibraryId{};
        const auto firstNumber=integer(c(workNumber),0),firstVersion=integer(c(versionNumber),1);
        startJob([this,selected,names,extra,titleValue,alias,mode,chosenId,firstNumber,firstVersion]
                 (CollectionStore& db,std::stop_token stop) {
            auto current=db.snapshot();
            std::vector<LibraryId> artists;
            std::set<std::wstring> savedNames;
            for(const auto& name:names) {
                if(!savedNames.insert(name).second) continue;
                auto found=std::ranges::find(current.artists,name,&Artist::name);
                Artist a;
                if(found!=current.artists.end()) a=*found;
                a.name=name;if(a.fileName.empty()) a.fileName=name;
                if(names.size()==1&&!alias.empty()) a.aliases=alias;
                const auto id=db.saveArtist(a);
                artists.push_back(id);
            }
            LibraryId common=chosenId;
            if(mode==2&&common.empty()) throw LibraryError("Choose an existing artwork");
            if(mode==2) {
                const auto found=std::ranges::find(current.artworks,common,&Artwork::id);
                if(found==current.artworks.end()) throw LibraryError("The selected artwork no longer exists");
                auto chosenWork=*found;
                if(!names.empty()) chosenWork.artists=artists;
                if(!titleValue.empty()) chosenWork.title=titleValue;
                if(firstNumber>0) chosenWork.number=firstNumber;
                db.saveArtwork(chosenWork);
            }
            if(mode==1) {
                Artwork a;
                a.artists=artists;a.title=titleValue;
                a.number=firstNumber?firstNumber:db.nextArtworkNumber(artists.empty()?LibraryId{}:artists.front());
                common=db.saveArtwork(a);
            }
            std::int64_t version=std::max<std::int64_t>(1,firstVersion),number=firstNumber;
            if(mode==2) version=std::max(version,db.nextVersionNumber(common));
            std::set<LibraryId> changedWorks;
            for(auto file:selected) {
                if(stop.stop_requested()) break;
                LibraryId workId=mode==3?LibraryId{}:common;
                if(mode==3) {
                    const auto found=std::ranges::find(current.artworks,file.artworkId,&Artwork::id);
                    if(found!=current.artworks.end()) {
                        workId=found->id;
                        if(changedWorks.insert(workId).second) {
                            auto original=*found;
                            original.artists=artists;
                            db.saveArtwork(original);
                        }
                    }
                }
                if(mode==0||(mode==3&&workId.empty())) {
                    Artwork a;a.artists=artists;a.title=titleValue.empty()?file.path.stem().wstring():titleValue;
                    a.number=number?number++:db.nextArtworkNumber(artists.empty()?LibraryId{}:artists.front());
                    workId=db.saveArtwork(a);
                }
                const auto existingVersion=std::ranges::find(current.versions,file.versionId,&MediaVersion::id);
                if(mode!=3||existingVersion==current.versions.end()||existingVersion->artworkId!=workId) {
                    MediaVersion v;v.artworkId=workId;
                    v.number=mode==0?std::max<std::int64_t>(1,firstVersion):mode==3?db.nextVersionNumber(workId):version++;
                    v.main=v.number==1;
                    file.versionId=db.saveVersion(v);
                }
                file.artworkId=workId;
                for(const auto& [key,value]:extra) file.fields.insert_or_assign(key,value);
                db.saveFile(file);
            }
            pending.message=L"Метаданные сохранены";
        },L"Сохранение метаданных…");
    }
    void savePreset(bool duplicate) {
        auto value=naming;
        value.name=trim(text(c(preset)));
        if(value.name.empty()) throw LibraryError("Enter a preset name");
        value.authorSeparator=text(c(authorJoin));
        value.counterStart=integer(c(counterStart),1);
        value.counterScope=static_cast<CounterScope>(std::max<LRESULT>(0,SendMessageW(c(counterScope),CB_GETCURSEL,0,0)));
        if(duplicate) {value.id.clear();value.name+=L" — копия";}
        const auto json=wide(serializeTemplate(value));
        naming=value;
        SetWindowTextW(c(preset),value.name.c_str());
        startJob([this,value,json](CollectionStore& db,std::stop_token) {
            const auto id=db.savePreset({value.id,value.name,json});
            db.setSetting(L"lastPreset",id);
            pending.savedPresetId=id;
        },L"Сохранение шаблона…");
    }
    void createPreview() {
        const auto selected=selectedRecords();
        if(selected.empty()) throw LibraryError("Select files in the catalog first");
        auto value=naming;
        value.authorSeparator=text(c(authorJoin));value.counterStart=integer(c(counterStart),1);
        value.counterScope=static_cast<CounterScope>(std::max<LRESULT>(0,SendMessageW(c(counterScope),CB_GETCURSEL,0,0)));
        const auto snapshot=data;
        const auto generation=std::pair{selectionGeneration,directoryGeneration};
        startJob([this,selected,value,snapshot,generation](CollectionStore&,std::stop_token) {
            const auto names=renderNames(value,snapshot,selected);
            std::vector<RenameProposal> proposals;
            for(std::size_t i=0;i<selected.size();++i) proposals.push_back({selected[i].id,selected[i].path,names[i],true});
            pending.preview=previewRenames(proposals);
            pending.previewGeneration=generation;
        },L"Проверка имён и конфликтов…");
    }
    void fillPreview() {
        filling=true;
        ListView_DeleteAllItems(c(previewList));
        shown.clear();
        const auto filter=text(c(previewSearch));
        for(std::size_t i=0;i<preview.size();++i) {
            const auto& row=preview[i];
            if(!filter.empty()&&row.proposal.source.filename().wstring().find(filter)==std::wstring::npos&&
                row.proposal.newName.find(filter)==std::wstring::npos) continue;
            const int index=static_cast<int>(shown.size());
            shown.push_back(i);
            const auto original=row.proposal.source.filename().wstring();
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=index;item.pszText=const_cast<wchar_t*>(original.c_str());
            ListView_InsertItem(c(previewList),&item);
            ListView_SetItemText(c(previewList),index,1,const_cast<wchar_t*>(row.proposal.newName.c_str()));
            const auto state=row.ready?L"Готов":row.problem.c_str();
            ListView_SetItemText(c(previewList),index,2,const_cast<wchar_t*>(state));
            ListView_SetCheckState(c(previewList),index,row.proposal.enabled);
        }
        filling=false;
    }
    void fillHistory() {
        filling=true;
        ListView_DeleteAllItems(c(historyList));
        for(std::size_t i=0;i<operations.size();++i) {
            auto& operation=operations[i];
            LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=operation.created.data();
            ListView_InsertItem(c(historyList),&item);
            ListView_SetItemText(c(historyList),item.iItem,1,operation.status.data());
            const auto count=std::to_wstring(operation.count);
            ListView_SetItemText(c(historyList),item.iItem,2,const_cast<wchar_t*>(count.c_str()));
            if(operation.id==selectedOperationId) {
                ListView_SetItemState(c(historyList),item.iItem,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            }
        }
        filling=false;
    }
    void showOperationPaths(const LibraryId& id) {
        if(busy()) return;
        selectedOperationId=id;
        startJob([this,id](CollectionStore& db,std::stop_token) {
            std::wstring details;
            for(const auto& [source,target]:db.operationPaths(id)) {
                if(!details.empty()) details+=L"\r\n\r\n";
                details+=source.wstring()+L"\r\n→ "+target.wstring();
            }
            pending.operationDetails=std::move(details);
        },L"Чтение истории…");
    }
    void rename() {
        const std::set<fs::path> selected(selection.begin(),selection.end());
        std::vector<RenameProposal> proposals;
        for(const auto& row:preview) if(row.proposal.enabled) {
            if(!selected.contains(row.proposal.source)) throw LibraryError("Selection changed. Rebuild the rename preview");
            proposals.push_back(row.proposal);
        }
        if(proposals.empty()) throw LibraryError("Build a rename preview first");
        const auto prompt=L"Переименовать "+std::to_wstring(proposals.size())+L" выбранных файлов?";
        if(MessageBoxW(window,prompt.c_str(),L"Переименование",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        before();
        startJob([this,proposals](CollectionStore& db,std::stop_token stop) {
            pending.files=db.rename(proposals,stop);
            pending.message=pending.files->message;
        },L"Переименование…");
    }
    void reverse(bool recovery) {
        const int row=ListView_GetNextItem(c(historyList),-1,LVNI_SELECTED);
        if(row<0||row>=static_cast<int>(operations.size())) throw LibraryError("Select an operation");
        const auto operation=operations[static_cast<std::size_t>(row)];
        if(MessageBoxW(window,recovery?L"Восстановить исходные имена незавершённой операции?":
            L"Отменить выбранное переименование?",L"История",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        before();
        startJob([this,operation,recovery](CollectionStore& db,std::stop_token stop) {
            pending.files=recovery?db.recover(operation.id,stop):db.undo(operation.id,stop);
            pending.message=pending.files->message;
        },L"Восстановление имён…");
    }
    void command(int id,int notification) {
        if(id>=tabAssign&&id<=tabHistory) {tab=id-tabAssign;scrollOffset=0;layout();return;}
        if(id==previewSearch&&notification==EN_CHANGE) {fillPreview();return;}
        if(id==cancelTask&&notification==BN_CLICKED) {worker.request_stop();return;}
        if(busy()) return;
        if(id==work&&(notification==CBN_SELCHANGE||notification==CBN_EDITCHANGE)) {
            const auto row=SendMessageW(c(work),CB_GETCURSEL,0,0);
            selectedWorkId=notification==CBN_SELCHANGE&&row>=0&&row<static_cast<LRESULT>(data.artworks.size())
                ?data.artworks[static_cast<std::size_t>(row)].id:LibraryId{};
            return;
        }
        if(id==preset&&notification==CBN_SELCHANGE) {
            const auto index=SendMessageW(c(preset),CB_GETCURSEL,0,0);
            if(index>=0&&index<static_cast<LRESULT>(presets.size())) {
                const auto& saved=presets[static_cast<std::size_t>(index)];
                naming=parseTemplate(utf8(saved.json));naming.id=saved.id;fillBlocks();fillGlobals();
            }
            return;
        }
        if(id==blocks&&notification==LBN_SELCHANGE) {selectBlock();return;}
        if(notification!=BN_CLICKED) return;
        switch(id) {
        case assign:assignMetadata();break;
        case presetSave:savePreset(false);break;
        case presetDuplicate:savePreset(true);break;
        case presetDelete: {
            const auto presetId=naming.id;
            if(presetId.empty()) break;
            startJob([presetId](CollectionStore& db,std::stop_token){db.deletePreset(presetId);},L"Удаление пресета…");
            naming=defaultArtistTemplate();fillBlocks();fillGlobals();
            SetWindowTextW(c(preset),naming.name.c_str());break;
        }
        case presetImport: {
            const auto path=pick(window,false,{});
            if(!path) break;
            std::ifstream input(*path,std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(input)),{});
            naming=parseTemplate(contents);naming.id.clear();
            SetWindowTextW(c(preset),naming.name.c_str());fillBlocks();fillGlobals();break;
        }
        case presetExport: {
            auto value=naming;value.name=text(c(preset));value.authorSeparator=text(c(authorJoin));
            value.counterStart=integer(c(counterStart),1);
            value.counterScope=static_cast<CounterScope>(std::max<LRESULT>(0,SendMessageW(c(counterScope),CB_GETCURSEL,0,0)));
            const auto path=pick(window,true,L"Viewery-preset.json");
            if(!path) break;
            std::ofstream output(*path,std::ios::binary);
            output<<serializeTemplate(value);
            if(!output) throw LibraryError("Cannot write preset JSON");
            break;
        }
        case blockAdd: {
            NameBlock block;
            block.kind=static_cast<NameBlockKind>(std::max<LRESULT>(0,SendMessageW(c(blockKind),CB_GETCURSEL,0,0)));
            if(block.kind==NameBlockKind::separator) block.text=L"_";
            naming.blocks.push_back(block);fillBlocks();
            SendMessageW(c(blocks),LB_SETCURSEL,naming.blocks.size()-1,0);selectBlock();break;
        }
        case blockApply:applyBlock();break;
        case blockRemove: {
            const auto index=SendMessageW(c(blocks),LB_GETCURSEL,0,0);
            if(index>=0&&index<static_cast<LRESULT>(naming.blocks.size())) naming.blocks.erase(naming.blocks.begin()+index);
            fillBlocks();break;
        }
        case blockUp:case blockDown: {
            const auto index=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
            const auto next=index+(id==blockUp?-1:1);
            if(index>=0&&next>=0&&next<static_cast<int>(naming.blocks.size())) {
                std::swap(naming.blocks[static_cast<std::size_t>(index)],naming.blocks[static_cast<std::size_t>(next)]);
                fillBlocks();SendMessageW(c(blocks),LB_SETCURSEL,next,0);selectBlock();
            }
            break;
        }
        case makePreview:createPreview();break;
        case editName: {
            const int row=ListView_GetNextItem(c(previewList),-1,LVNI_SELECTED);
            if(row<0||row>=static_cast<int>(shown.size())) break;
            auto proposals=std::vector<RenameProposal>{};
            const auto changedRow=shown[static_cast<std::size_t>(row)];
            const auto generation=std::pair{selectionGeneration,directoryGeneration};
            for(std::size_t i=0;i<preview.size();++i) {
                auto proposal=preview[i].proposal;
                if(i==changedRow) proposal.newName=text(c(newName));
                proposals.push_back(std::move(proposal));
            }
            startJob([this,proposals,generation](CollectionStore&,std::stop_token){
                pending.preview=previewRenames(proposals);pending.previewGeneration=generation;},
                     L"Проверка имён…");break;
        }
        case apply:rename();break;
        case undo:reverse(false);break;
        case recover:reverse(true);break;
        case dbBackup:case metaExport:case metaImport: {
            const auto path=pick(window,id!=metaImport,id==dbBackup?L"Viewery-collection.sqlite3":L"Viewery-collection.json",id==dbBackup);
            if(!path) break;
            startJob([id,path](CollectionStore& db,std::stop_token) {
                if(id==dbBackup) db.backup(*path);
                else if(id==metaExport) db.exportMetadata(*path);
                else db.importMetadata(*path);
            },L"Обработка базы коллекции…");break;
        }
        default:break;
        }
    }
    static LRESULT CALLBACK blocksProc(HWND child,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& self=*reinterpret_cast<Impl*>(data);
        if(msg==WM_LBUTTONDOWN&&!self.busy()) {
            self.dragBlock=LOWORD(SendMessageW(child,LB_ITEMFROMPOINT,0,l));
        } else if(msg==WM_LBUTTONUP&&self.dragBlock>=0&&!self.busy()) {
            const int from=std::exchange(self.dragBlock,-1);
            const int to=LOWORD(SendMessageW(child,LB_ITEMFROMPOINT,0,l));
            if(from!=to&&from<static_cast<int>(self.naming.blocks.size())&&to<static_cast<int>(self.naming.blocks.size())) {
                auto block=std::move(self.naming.blocks[static_cast<std::size_t>(from)]);
                self.naming.blocks.erase(self.naming.blocks.begin()+from);
                self.naming.blocks.insert(self.naming.blocks.begin()+to,std::move(block));
                self.fillBlocks();SendMessageW(child,LB_SETCURSEL,to,0);self.selectBlock();
            }
        }
        return DefSubclassProc(child,msg,w,l);
    }
    LRESULT dispatch(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
        try {
            switch(msg) {
            case WM_VSCROLL: {
                SCROLLINFO scroll{sizeof(scroll),SIF_TRACKPOS};
                GetScrollInfo(hwnd,SB_VERT,&scroll);
                const int action=LOWORD(w);
                if(action==SB_LINEUP) scrollOffset-=28;
                if(action==SB_LINEDOWN) scrollOffset+=28;
                if(action==SB_PAGEUP) scrollOffset-=180;
                if(action==SB_PAGEDOWN) scrollOffset+=180;
                if(action==SB_THUMBTRACK||action==SB_THUMBPOSITION) scrollOffset=scroll.nTrackPos;
                scrollOffset=std::clamp(scrollOffset,0,scrollLimit);layout();return 0;
            }
            case WM_MOUSEWHEEL:
                scrollOffset=std::clamp(scrollOffset-GET_WHEEL_DELTA_WPARAM(w)*45/WHEEL_DELTA,0,scrollLimit);
                layout();return 0;
            case WM_SIZE:layout();return 0;
            case finished:finishJob();return 0;
            case WM_COMMAND:command(LOWORD(w),HIWORD(w));return 0;
            case WM_ERASEBKGND:return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);fill(dc,paint.rcPaint,bg);EndPaint(hwnd,&paint);return 0;
            }
            case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX: {
                HDC dc=reinterpret_cast<HDC>(w);
                SetTextColor(dc,fg);SetBkColor(dc,msg==WM_CTLCOLORSTATIC?bg:inputColor);
                return reinterpret_cast<LRESULT>(msg==WM_CTLCOLORSTATIC?brush:editBrush);
            }
            case WM_DRAWITEM: {
                auto& draw=*reinterpret_cast<DRAWITEMSTRUCT*>(l);
                const bool active=(draw.CtlID==static_cast<UINT>(tabAssign+tab));
                fill(draw.hDC,draw.rcItem,active?RGB(42,37,69):inputColor);
                const auto old=SelectObject(draw.hDC,font);
                SetBkMode(draw.hDC,TRANSPARENT);
                SetTextColor(draw.hDC,(draw.itemState&ODS_DISABLED)?muted:active?accent:fg);
                auto value=text(draw.hwndItem);
                DrawTextW(draw.hDC,value.c_str(),static_cast<int>(value.size()),&draw.rcItem,
                          DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                SelectObject(draw.hDC,old);return TRUE;
            }
            case WM_NOTIFY: {
                const auto* header=reinterpret_cast<const NMHDR*>(l);
                if(header->idFrom==previewList&&header->code==LVN_ITEMCHANGED&&!filling) {
                    const auto& event=*reinterpret_cast<const NMLISTVIEW*>(l);
                    if(event.iItem>=0&&event.iItem<static_cast<int>(shown.size())) {
                        auto& row=preview[shown[static_cast<std::size_t>(event.iItem)]];
                        row.proposal.enabled=ListView_GetCheckState(c(previewList),event.iItem)!=FALSE;
                        if(event.uNewState&LVIS_SELECTED) SetWindowTextW(c(newName),row.proposal.newName.c_str());
                    }
                } else if(header->idFrom==previewList&&header->code==LVN_COLUMNCLICK) {
                    const int column=reinterpret_cast<const NMLISTVIEW*>(l)->iSubItem;
                    std::ranges::sort(preview,[column](const RenamePreview& a,const RenamePreview& b) {
                        return column==1?a.proposal.newName<b.proposal.newName:a.proposal.source<b.proposal.source;
                    });fillPreview();
                } else if(header->idFrom==historyList&&header->code==LVN_ITEMCHANGED&&!filling&&!busy()) {
                    const int row=ListView_GetNextItem(c(historyList),-1,LVNI_SELECTED);
                    if(row>=0&&row<static_cast<int>(operations.size())&&
                        operations[static_cast<std::size_t>(row)].id!=selectedOperationId) {
                        showOperationPaths(operations[static_cast<std::size_t>(row)].id);
                    }
                }
                break;
            }
            }
        } catch(const std::exception& error) {
            SetWindowTextW(c(status),wide(error.what()).c_str());
        }
        return DefWindowProcW(hwnd,msg,w,l);
    }
};

RenameStudio::RenameStudio(HWND owner,UINT changedMessage,BeforeFilesChange beforeChange,fs::path database)
    :impl_(std::make_unique<Impl>(owner,changedMessage,std::move(beforeChange),std::move(database))) {
    SetWindowSubclass(impl_->c(blocks),Impl::blocksProc,1,reinterpret_cast<DWORD_PTR>(impl_.get()));
}
RenameStudio::~RenameStudio()=default;
HWND RenameStudio::window() const {return impl_->window;}
void RenameStudio::show(RECT bounds,UINT dpi,HFONT font) {
    impl_->dpi=dpi;impl_->font=font;
    for(const auto& [id,control]:impl_->controls) {
        (void)id;SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    }
    SetWindowPos(impl_->window,HWND_TOP,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                 SWP_NOACTIVATE|SWP_SHOWWINDOW);
    impl_->layout();
}
void RenameStudio::hide() {ShowWindow(impl_->window,SW_HIDE);}
bool RenameStudio::busy() const {return impl_->busy();}
void RenameStudio::cancel() {impl_->worker.request_stop();}
void RenameStudio::setFiles(std::vector<fs::path> directory,std::vector<fs::path> selected) {
    const bool changed=directory!=impl_->directory;
    if(changed) {
        ++impl_->directoryGeneration;
        impl_->preview.clear();impl_->fillPreview();
    }
    impl_->directory=std::move(directory);
    if(selected!=impl_->selection) impl_->updateSelection(std::move(selected));
    if(changed||(!impl_->busy()&&!impl_->store)) impl_->beginIndex();
}
const CollectionSnapshot& RenameStudio::snapshot() const {return impl_->data;}
std::optional<RenameResult> RenameStudio::takeFileChanges() {return std::exchange(impl_->fileResult,std::nullopt);}
}
