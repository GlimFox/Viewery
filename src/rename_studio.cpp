#include "rename_studio.hpp"
#include "rename_template.hpp"

#include <commctrl.h>
#include <dwmapi.h>
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
constexpr UINT validateQueueMessage = WM_APP + 432;
constexpr UINT deferredUiMessage = WM_APP + 433;
constexpr COLORREF bg = RGB(23,26,33), fg = RGB(241,242,247), muted = RGB(149,155,170);
constexpr COLORREF accent = RGB(155,140,255), inputColor = RGB(36,40,52);
enum Id {
    tabAssign=1000, tabTemplate, tabPreview, tabHistory, summary, status,
    artist, title, work, assignment, workNumber, versionNumber, assign, fields, aliases,
    preset, presetSave, presetDuplicate, presetDelete, presetImport, presetExport,
    blocks, blockKind, blockAdd, blockRemove, blockUp, blockDown, blockText, blockDigits,
    blockPrefix, blockSuffix, blockFallback, blockCase, blockApply, authorJoin,
    counterStart, counterScope, previewList, previewSearch, newName, editName, makePreview,
    apply, historyList, undo, recover, cancelTask, dbBackup, metaExport, metaImport, historyDetails,
    closePanel, advancedToggle, presetMenu, liveExample, scrollViewport, scrollContent, newPreset,
    dataAdvanced, queueRemove, queueClear, batchPresetName, artistRegistry=RenameStudio::artistsCommandId
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
bool sameName(std::wstring_view first,std::wstring_view second) {
    return CompareStringOrdinal(first.data(),static_cast<int>(first.size()),
        second.data(),static_cast<int>(second.size()),TRUE)==CSTR_EQUAL;
}
const Artist* namedArtist(const std::vector<Artist>& artists,std::wstring_view token) {
    const auto unique=[&](auto matches) {
        const Artist* result=nullptr;
        for(const auto& entry:artists) if(matches(entry)) {
            if(result&&result->id!=entry.id) throw LibraryError("Artist name or alias is ambiguous. Choose a name from the list.");
            result=&entry;
        }
        return result;
    };
    if(const auto found=unique([&](const Artist& entry){return sameName(entry.name,token);})) return found;
    if(const auto found=unique([&](const Artist& entry){return sameName(entry.fileName,token);})) return found;
    return unique([&](const Artist& entry) {
        auto aliasesValue=entry.aliases;
        std::ranges::replace_if(aliasesValue,[](wchar_t ch){return ch==L','||ch==L'\r'||ch==L'\n';},L';');
        const auto names=split(aliasesValue,L';');
        return std::ranges::any_of(names,[&](const auto& name){return sameName(name,token);});
    });
}
std::wstring refreshedArtistInput(std::wstring_view value,const std::vector<Artist>& previous,
                                const std::vector<Artist>& current) {
    auto names=split(value,L';');
    bool changed=false;
    for(auto& name:names) {
        const Artist* old=nullptr;
        try {old=namedArtist(previous,name);} catch(const LibraryError&) {continue;}
        if(!old) continue;
        const auto found=std::ranges::find(current,old->id,&Artist::id);
        if(found==current.end()) {name.clear();changed=true;}
        else {
            const Artist* stillBound=nullptr;
            try {stillBound=namedArtist(current,name);} catch(const LibraryError&) {}
            if(found->name!=old->name||!stillBound||stillBound->id!=old->id) {name=found->name;changed=true;}
        }
    }
    if(!changed) return std::wstring(value);
    std::wstring result;
    for(const auto& name:names) if(!name.empty()) {if(!result.empty()) result+=L"; ";result+=name;}
    return result;
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
    HWND owner=nullptr,window=nullptr,viewport=nullptr,content=nullptr,tips=nullptr,editorHost=nullptr;
    UINT changed=0,dpi=96;
    HFONT font=nullptr,iconFont=nullptr;
    HBRUSH brush=CreateSolidBrush(bg),editBrush=CreateSolidBrush(inputColor);
    std::map<int,HWND> controls;
    std::vector<HWND> labels;
    int tab=0,dragBlock=-1,scrollOffset=0,scrollLimit=0,contentHeight=0;
    POINT dragOrigin{};
    bool filling=false,indexPending=false,refreshPending=false,layingOut=false,dragging=false,advanced=false;
    bool templateOnly=false,dataExpanded=false;
    bool batchQueued=false;
    int deferredUiAction=0;
    bool deferredUiPosted=false;
    Impl* templateParent=nullptr;
    std::unique_ptr<Impl> editor;
    HFONT editorFont=nullptr;
    RECT shownBounds{};
    bool hasShownBounds=false;
    std::size_t captionIndex=0;
    std::uint64_t selectionGeneration=0,directoryGeneration=0;
    std::vector<fs::path> directory,selection,indexed;
    CollectionSnapshot data;
    std::vector<LibraryPreset> presets;
    RenameTemplate naming=defaultArtistTemplate();
    std::vector<LibraryId> artistChoices;
    LibraryId selectedArtistId;
    std::wstring selectedArtistText;
    bool selectingArtist=false;
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
    std::wstring tooltipText;
    int dp(int value) const { return MulDiv(value,static_cast<int>(dpi),96); }
    HWND c(int id) const { const auto found=controls.find(id);return found==controls.end()?nullptr:found->second; }
    bool busy() const { return worker.joinable()||(editor&&editor->worker.joinable()); }

    Impl(HWND parent,UINT msg,BeforeFilesChange callback,fs::path databasePath,bool onlyTemplate=false)
        :owner(parent),changed(msg),templateOnly(onlyTemplate),database(std::move(databasePath)),before(std::move(callback)) {
        try {
            const INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES};
            if(!InitCommonControlsEx(&common)) throw LibraryError("Cannot initialize native collection controls");
            static const ATOM klass=[] {
                WNDCLASSW wc{};
                wc.hInstance=GetModuleHandleW(nullptr);
                wc.lpfnWndProc=procedure;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                wc.lpszClassName=L"Viewery.RenameStudio.1";
                return RegisterClassW(&wc);
            }();
            if(!klass) throw LibraryError("Cannot register Rename Studio");
            window=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.RenameStudio.1",L"Rename Studio",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
                                   0,0,0,0,parent,nullptr,GetModuleHandleW(nullptr),this);
            if(!window) throw LibraryError("Cannot create Rename Studio");
            viewport=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.RenameStudio.1",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|WS_VSCROLL,
                0,0,0,0,window,reinterpret_cast<HMENU>(scrollViewport),GetModuleHandleW(nullptr),this);
            content=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.RenameStudio.1",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
                0,0,0,0,viewport,reinterpret_cast<HMENU>(scrollContent),GetModuleHandleW(nullptr),this);
            if(!viewport||!content) throw LibraryError("Cannot create Rename Studio viewport");
            build();
            tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,L"",WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
                CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(tips) {
                SendMessageW(tips,TTM_SETMAXTIPWIDTH,0,500);
                for(int id:{closePanel,preset,liveExample,status,blocks,blockUp,blockDown,artistRegistry}) {
                    TTTOOLINFOW info{sizeof(info)};info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;info.hwnd=window;
                    info.uId=reinterpret_cast<UINT_PTR>(c(id));info.lpszText=LPSTR_TEXTCALLBACKW;
                    SendMessageW(tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
                }
            }
        } catch(...) {
            if(window) DestroyWindow(window);
            DeleteObject(brush);DeleteObject(editBrush);
            throw;
        }
    }
    ~Impl() {
        editor.reset();
        if(editorHost) DestroyWindow(editorHost);
        if(editorFont) DeleteObject(editorFont);
        worker.request_stop();
        if(worker.joinable()) worker.join();
        if(window) DestroyWindow(window);
        if(iconFont) DeleteObject(iconFont);
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
        const bool fixed=id<=status||id==closePanel||id==cancelTask;
        HWND result=CreateWindowExW(0,type,value,WS_CHILD|WS_CLIPSIBLINGS|WS_TABSTOP|style,0,0,0,0,fixed?window:content,
                                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!result) throw LibraryError("Cannot create Rename Studio control "+std::to_string(id));
        controls[id]=result;
        if(wcscmp(type,WC_LISTVIEWW)==0) {
            ListView_SetBkColor(result,bg);
            ListView_SetTextBkColor(result,bg);
            ListView_SetTextColor(result,fg);
            ListView_SetExtendedListViewStyle(result,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        }
        SetWindowTheme(result,L"DarkMode_Explorer",nullptr);
        SetWindowSubclass(result,inputProc,2,reinterpret_cast<DWORD_PTR>(this));
        if(wcscmp(type,L"COMBOBOX")==0) {
            COMBOBOXINFO info{sizeof(info)};
            if(GetComboBoxInfo(result,&info)&&info.hwndItem&&info.hwndItem!=result) {
                SetWindowSubclass(info.hwndItem,inputProc,2,reinterpret_cast<DWORD_PTR>(this));
            }
        }
        return result;
    }
    void button(int id,const wchar_t* value) { create(id,L"BUTTON",value,BS_OWNERDRAW); }
    void edit(int id,const wchar_t* value=L"",DWORD extra=0) { create(id,L"EDIT",value,ES_AUTOHSCROLL|extra); }
    void combo(int id,bool editable=false) {
        create(id,L"COMBOBOX",L"",CBS_HASSTRINGS|WS_VSCROLL|
            (editable?CBS_DROPDOWN:CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED));
    }
    void item(int id,const wchar_t* value) { SendMessageW(c(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value)); }
    void column(int id,int index,const wchar_t* name,int width) {
        LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
        col.pszText=const_cast<wchar_t*>(name);col.cx=dp(width);col.iSubItem=index;
        ListView_InsertColumn(c(id),index,&col);
    }
    void build() {
        button(tabAssign,L"Пачка");button(tabTemplate,L"Шаблоны…");
        button(tabPreview,L"Очередь");button(tabHistory,L"История");
        button(closePanel,L"Закрыть");
        create(summary,L"STATIC",L"Выбрано 0 файлов",0);
        create(status,L"STATIC",L"",SS_LEFT);
        combo(artist,true);combo(work,true);edit(title);
        button(RenameStudio::artistsCommandId,L"База художников");
        edit(workNumber);edit(versionNumber,L"1");
        combo(assignment);item(assignment,L"Разные работы");
        item(assignment,L"Версии одной работы");item(assignment,L"К существующей работе");
        item(assignment,L"Только художник");
        SendMessageW(c(assignment),CB_SETCURSEL,0,0);
        edit(aliases);edit(fields,L"",ES_MULTILINE|ES_WANTRETURN|WS_VSCROLL);
        button(assign,L"Назначить выбранным");
        button(dbBackup,L"Копия базы");button(metaExport,L"Экспорт JSON");button(metaImport,L"Импорт JSON");
        combo(preset,true);button(presetSave,L"Сохранить");button(presetDuplicate,L"Копия");
        button(presetDelete,L"Удалить");button(presetImport,L"Импорт");button(presetExport,L"Экспорт");
        create(blocks,L"LISTBOX",L"",LBS_NOTIFY|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS);
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
        button(advancedToggle,L"Дополнительные параметры  ▾");
        button(presetMenu,L"···");
        create(liveExample,L"STATIC",L"",SS_OWNERDRAW);
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
        button(dataAdvanced,L"Дополнительные данные  ▾");
        button(queueRemove,L"Убрать строку");button(queueClear,L"Очистить очередь");
        create(batchPresetName,L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS);
        fillBlocks();fillGlobals();
    }
    bool dropdownOpen() const {
        for(int id:{artist,work,assignment,preset,blockKind,blockCase,counterScope}) {
            if(SendMessageW(c(id),CB_GETDROPPEDSTATE,0,0)) return true;
        }
        return false;
    }
    void close() {deferredUiAction=0;PostMessageW(owner,WM_COMMAND,RenameStudio::closeCommandId,0);}
    void scheduleDeferredUi() {
        if(deferredUiAction&&!deferredUiPosted&&!busy()&&!indexPending&&directory==indexed) {
            deferredUiPosted=PostMessageW(window,deferredUiMessage,0,0)!=FALSE;
        }
    }
    bool postponeUi(int action) {
        if(!busy()&&!indexPending&&directory==indexed) return false;
        deferredUiAction=action;
        SetWindowTextW(c(status),L"Откроется после подготовки коллекции…");
        if(!busy()) beginIndex();
        return true;
    }
    void cancelJobs() {
        deferredUiAction=0;worker.request_stop();
        if(templateParent) templateParent->deferredUiAction=0;
        if(editor) {editor->deferredUiAction=0;editor->worker.request_stop();}
    }
    static LRESULT CALLBACK editorProcedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self) return DefWindowProcW(hwnd,msg,w,l);
        if(msg==WM_SIZE&&self->editor) {self->sizeEditor();return 0;}
        if(msg==WM_DPICHANGED) {
            const auto bounds=*reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                SWP_NOZORDER|SWP_NOACTIVATE);
            self->sizeEditor();return 0;
        }
        if(msg==WM_GETMINMAXINFO) {
            auto& limits=*reinterpret_cast<MINMAXINFO*>(l);
            const UINT scale=GetDpiForWindow(hwnd);
            limits.ptMinTrackSize={MulDiv(440,static_cast<int>(scale),96),MulDiv(430,static_cast<int>(scale),96)};
            return 0;
        }
        if(msg==WM_CLOSE||(msg==WM_COMMAND&&LOWORD(w)==RenameStudio::closeCommandId)) {
            if(self->deferredUiAction==tabTemplate) self->deferredUiAction=0;
            ShowWindow(hwnd,SW_HIDE);return 0;
        }
        if(msg==self->changed&&self->editor) {
            self->syncTemplateFromEditor();
            self->layout();
            PostMessageW(self->owner,self->changed,0,0);
            if(self->indexPending&&!self->busy()) self->beginIndex();
            if(self->refreshPending&&!self->busy()) self->beginRefresh();
            self->scheduleDeferredUi();
            return 0;
        }
        return DefWindowProcW(hwnd,msg,w,l);
    }
    void setFont(HFONT value,UINT scale) {
        dpi=scale;font=value;
        for(const auto& [id,control]:controls) {
            (void)id;SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        }
        const auto previous=iconFont;
        iconFont=CreateFontW(-dp(17),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe Fluent Icons");
        SendMessageW(c(RenameStudio::artistsCommandId),WM_SETFONT,reinterpret_cast<WPARAM>(iconFont?iconFont:font),FALSE);
        if(previous) DeleteObject(previous);
        SendMessageW(c(blocks),LB_SETITEMHEIGHT,0,dp(32));
        for(int id:{assignment,blockKind,blockCase,counterScope}) {
            SendMessageW(c(id),CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),dp(22));
            SendMessageW(c(id),CB_SETITEMHEIGHT,0,dp(26));
        }
    }
    void sizeEditor() {
        if(!editor||!editorHost) return;
        const UINT scale=GetDpiForWindow(editorHost);
        if(!editorFont||editor->dpi!=scale) {
            const HFONT next=CreateFontW(-MulDiv(14,static_cast<int>(scale),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe UI");
            editor->setFont(next,scale);
            if(editorFont) DeleteObject(editorFont);
            editorFont=next;
        }
        RECT area{};GetClientRect(editorHost,&area);
        SetWindowPos(editor->window,nullptr,0,0,area.right,area.bottom,SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        editor->layout();
    }
    void syncTemplateFromEditor() {
        if(!editor) return;
        naming=editor->naming;
        naming.authorSeparator=text(editor->c(authorJoin));
        naming.counterStart=integer(editor->c(counterStart),1);
        const auto scope=SendMessageW(editor->c(counterScope),CB_GETCURSEL,0,0);
        if(scope>=0) naming.counterScope=static_cast<CounterScope>(scope);
        if(!editor->worker.joinable()) {
            if(!store&&editor->store) {store=editor->store;data=editor->data;}
            presets=editor->presets;
        }
        fillGlobals();
        SetWindowTextW(c(batchPresetName),naming.name.c_str());
        updateExample();
    }
    void openEditor() {
        if(templateOnly||postponeUi(tabTemplate)) return;
        if(!editorHost) {
            static const ATOM klass=[] {
                WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=editorProcedure;
                wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"Viewery.TemplateEditor.1";
                return RegisterClassW(&wc);
            }();
            if(!klass) throw LibraryError("Cannot register template editor");
            editorHost=CreateWindowExW(WS_EX_CONTROLPARENT,L"Viewery.TemplateEditor.1",L"Шаблоны имён",
                WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,dp(570),dp(790),owner,
                nullptr,GetModuleHandleW(nullptr),this);
            if(!editorHost) throw LibraryError("Cannot open template editor");
            const BOOL dark=TRUE;
            DwmSetWindowAttribute(editorHost,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
            editor=std::make_unique<Impl>(editorHost,changed,BeforeFilesChange{},database,true);
            editor->templateParent=this;
            editor->tab=1;
            SetWindowSubclass(editor->c(blocks),blocksProc,1,reinterpret_cast<DWORD_PTR>(editor.get()));
        }
        editor->data=data;editor->presets=presets;editor->naming=naming;editor->selection=selection;
        editor->directory=directory;editor->indexed=indexed;editor->store=store;
        editor->fillMetadata();editor->fillPresets();editor->fillBlocks();editor->fillGlobals();editor->selectBlock();
        SetWindowTextW(editor->c(summary),L"Шаблоны имён");
        sizeEditor();
        ShowWindow(editorHost,SW_SHOWNORMAL);SetForegroundWindow(editorHost);
        if(!editor->store) editor->beginIndex();
    }
    void scrollTo(int value) {
        if(dropdownOpen()) return;
        const int next=std::clamp(value,0,scrollLimit);
        if(next==scrollOffset) return;
        scrollOffset=next;
        SetScrollPos(viewport,SB_VERT,scrollOffset,TRUE);
        SetWindowPos(content,nullptr,0,-dp(scrollOffset),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        RedrawWindow(viewport,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    void position(int id,int x,int y,int width,int height,std::set<int>& visible) {
        visible.insert(id);
        SetWindowPos(c(id),nullptr,dp(x),dp(y),dp(std::max(1,width)),dp(std::max(1,height)),
                     SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW|SWP_NOREDRAW);
        const bool available=!busy()||id==cancelTask||id==closePanel||id<=status;
        EnableWindow(c(id),available);
    }
    void caption(const wchar_t* value,int x,int y,int width) {
        HWND label=nullptr;
        if(captionIndex<labels.size()) label=labels[captionIndex];
        else {
            label=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPSIBLINGS,0,0,0,0,
                                 content,nullptr,GetModuleHandleW(nullptr),nullptr);
            labels.push_back(label);
        }
        ++captionIndex;
        SetWindowTextW(label,value);
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        SetWindowPos(label,nullptr,dp(x),dp(y),dp(std::max(1,width)),dp(21),
                     SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW|SWP_NOREDRAW);
    }
    static bool numbered(NameBlockKind kind) {
        return kind==NameBlockKind::workNumber||kind==NameBlockKind::version||kind==NameBlockKind::counter;
    }
    void layout() {
        if(!viewport||!content||layingOut||dropdownOpen()) return;
        layingOut=true;
        RECT rect{};GetClientRect(window,&rect);
        const int width=MulDiv(rect.right,96,static_cast<int>(dpi));
        const int height=MulDiv(rect.bottom,96,static_cast<int>(dpi));
        const int bodyTop=templateOnly?46:82;
        const int bodyHeight=std::max(1,height-bodyTop-50);
        const auto assignmentMode=SendMessageW(c(assignment),CB_GETCURSEL,0,0);
        const bool existingWork=assignmentMode==2,artistOnly=assignmentMode==3;
        SetWindowPos(viewport,nullptr,0,dp(bodyTop),rect.right,dp(bodyHeight),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        contentHeight=tab==0?(dataExpanded?827:447)+(existingWork?62:0)-(artistOnly?62:0):tab==1?(advanced?910:582):tab==2?std::max(510,bodyHeight):std::max(440,bodyHeight);
        scrollLimit=std::max(0,contentHeight-bodyHeight);
        scrollOffset=std::clamp(scrollOffset,0,scrollLimit);
        SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
        scroll.nMin=0;scroll.nMax=contentHeight-1;scroll.nPage=static_cast<UINT>(bodyHeight);scroll.nPos=scrollOffset;
        SetScrollInfo(viewport,SB_VERT,&scroll,TRUE);
        RECT body{};GetClientRect(viewport,&body);
        const int bodyWidth=MulDiv(body.right,96,static_cast<int>(dpi));
        SetWindowPos(content,nullptr,0,-dp(scrollOffset),body.right,dp(contentHeight),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        captionIndex=0;
        std::set<int> visible;
        const auto put=[&](int id,int x,int y,int w,int h=30){position(id,x,y,w,h,visible);};
        const int inside=std::max(1,bodyWidth-24);
        for(int id:{artist,work,assignment,preset,blockKind,blockCase,counterScope}) {
            SendMessageW(c(id),CB_SETDROPPEDWIDTH,dp(std::max(inside,410)),0);
        }
        put(summary,12,12,std::max(1,width-112),23);
        put(closePanel,std::max(12,width-94),7,82,31);
        if(!templateOnly) {
            const int tabWidth=std::max(1,(width-33)/4);
            for(int index=0;index<4;++index) put(tabAssign+index,12+index*(tabWidth+3),43,tabWidth,29);
        }
        put(status,12,std::max(0,height-42),std::max(1,width-(busy()?150:24)),38);
        if(busy()) put(cancelTask,std::max(12,width-132),std::max(0,height-39),120,30);
        if(tab==0) {
            caption(L"Художник",12,6,inside);
            put(artist,12,29,std::max(1,inside-38),210);
            put(RenameStudio::artistsCommandId,bodyWidth-42,29,30,30);
            int y=68;
            if(!artistOnly) {caption(L"Название · пусто — исходное имя",12,y,inside);put(title,12,y+23,inside);y+=62;}
            caption(L"Выбранные файлы — это",12,y,inside);put(assignment,12,y+23,inside,180);y+=62;
            const wchar_t* explanation=assignmentMode==1?L"Одно название · версии v01, v02…":assignmentMode==2?L"Продолжить выбранную работу":
                artistOnly?L"Сохранить номера и названия работ":L"У каждого файла свой номер работы";
            caption(explanation,12,y,inside);y+=22;
            if(existingWork) {caption(L"Выберите работу",12,y,inside);put(work,12,y+23,inside,210);y+=62;}
            caption(L"Шаблон · редактируется в отдельном окне",12,y,inside);
            put(batchPresetName,12,y+23,inside,25);y+=62;
            caption(L"Пример будущего имени",12,y,inside);put(liveExample,12,y+23,inside,34);y+=68;
            put(makePreview,12,y,inside,34);SetWindowTextW(c(makePreview),L"Добавить пачку в очередь");y+=46;
            put(dataAdvanced,12,y,inside);y+=42;
            SetWindowTextW(c(dataAdvanced),dataExpanded?L"Дополнительные данные  ▴":L"Дополнительные данные  ▾");
            if(dataExpanded) {
                const int half=(inside-12)/2;
                caption(L"№ работы · авто",12,y,half);caption(L"Первая версия",24+half,y,half);
                put(workNumber,12,y+23,half);put(versionNumber,24+half,y+23,half);y+=62;
                caption(L"Псевдонимы художника",12,y,inside);put(aliases,12,y+23,inside);y+=62;
                caption(L"Поля файлов · ключ=значение",12,y,inside);put(fields,12,y+23,inside,73);y+=108;
                put(assign,12,y,inside,34);SetWindowTextW(c(assign),L"Сохранить только данные");y+=46;
                const int third=(inside-12)/3;
                put(dbBackup,12,y,third);put(metaExport,18+third,y,third);put(metaImport,24+third*2,y,third);
            }
        } else if(tab==1) {
            caption(L"Пресет · выберите или введите своё имя",12,6,inside);
            put(preset,12,29,std::max(1,inside-146),210);
            put(presetSave,bodyWidth-150,29,96);put(presetMenu,bodyWidth-48,29,36);
            caption(L"Будущее имя · пример выбранного файла",12,72,inside);
            put(liveExample,12,96,inside,45);
            caption(L"Порядок частей имени",12,151,inside);
            put(blocks,12,176,inside,160);
            put(blockUp,12,344,42);put(blockDown,60,344,42);put(blockRemove,108,344,100);
            put(blockKind,12,384,std::max(1,inside-108),240);put(blockAdd,bodyWidth-112,384,100);
            const int selected=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
            int y=427;
            if(selected>=0&&selected<static_cast<int>(naming.blocks.size())) {
                const auto kind=naming.blocks[static_cast<std::size_t>(selected)].kind;
                if(numbered(kind)) {
                    caption(L"Знаков в номере · 3 → 001",12,y,inside);put(blockDigits,12,y+23,104);
                    y+=62;
                } else if(kind==NameBlockKind::literal||kind==NameBlockKind::separator||kind==NameBlockKind::customField||kind==NameBlockKind::date) {
                    const wchar_t* label=kind==NameBlockKind::customField?L"Имя пользовательского поля":kind==NameBlockKind::date?L"Формат даты · например %Y-%m-%d":L"Текст разделителя или части имени";
                    caption(label,12,y,inside);put(blockText,12,y+23,inside);y+=62;
                } else {
                    caption(L"Значение берётся из метаданных файла",12,y,inside);y+=32;
                }
            } else {caption(L"Выберите часть имени, чтобы настроить её",12,y,inside);y+=32;}
            put(advancedToggle,12,y,inside);y+=42;
            if(advanced) {
                const int half=(inside-12)/2;
                caption(L"Текст перед частью",12,y,half);caption(L"Текст после части",24+half,y,half);
                put(blockPrefix,12,y+23,half);put(blockSuffix,24+half,y+23,half);y+=62;
                caption(L"Если значение пустое",12,y,inside);put(blockFallback,12,y+23,inside);y+=62;
                caption(L"Регистр текста",12,y,inside);put(blockCase,12,y+23,inside,160);y+=62;
                caption(L"Разделитель соавторов",12,y,half);caption(L"Счётчик начинается с",24+half,y,half);
                put(authorJoin,12,y+23,half);put(counterStart,24+half,y+23,half);y+=62;
                caption(L"Область счётчика",12,y,inside);put(counterScope,12,y+23,inside,160);y+=62;
            }
            put(makePreview,12,y,inside,34);
            SetWindowTextW(c(makePreview),L"Использовать этот шаблон");
            SetWindowTextW(c(advancedToggle),advanced?L"Дополнительные параметры  ▴":L"Дополнительные параметры  ▾");
        } else if(tab==2) {
            caption(L"Пачки готовы · проверьте имена перед запуском",12,6,inside);
            put(previewSearch,12,29,inside);
            const int listHeight=std::max(146,contentHeight-296);
            put(previewList,12,70,inside,listHeight);
            caption(L"Новое имя выбранной строки",12,82+listHeight,inside);
            put(newName,12,105+listHeight,std::max(1,inside-116));put(editName,bodyWidth-120,105+listHeight,108);
            put(queueRemove,12,147+listHeight,(inside-8)/2);put(queueClear,20+(inside-8)/2,147+listHeight,(inside-8)/2);
            put(apply,12,189+listHeight,inside,34);SetWindowTextW(c(apply),L"Переименовать всю очередь");
        } else {
            const int listHeight=std::max(140,contentHeight-248);
            put(historyList,12,6,inside,listHeight);
            caption(L"Пути выбранной операции",12,18+listHeight,inside);
            put(historyDetails,12,41+listHeight,inside,108);
            put(undo,12,161+listHeight,(inside-8)/2,34);put(recover,20+(inside-8)/2,161+listHeight,(inside-8)/2,34);
        }
        for(std::size_t i=captionIndex;i<labels.size();++i) ShowWindow(labels[i],SW_HIDE);
        for(const auto& [id,control]:controls) if(!visible.contains(id)) ShowWindow(control,SW_HIDE);
        if(c(previewList)) {
            const int usable=std::max(dp(250),dp(inside)-GetSystemMetrics(SM_CXVSCROLL)-4);
            ListView_SetColumnWidth(c(previewList),0,usable*35/100);ListView_SetColumnWidth(c(previewList),1,usable*41/100);
            ListView_SetColumnWidth(c(previewList),2,usable*24/100);
            ListView_SetColumnWidth(c(historyList),0,usable*48/100);ListView_SetColumnWidth(c(historyList),1,usable*32/100);
            ListView_SetColumnWidth(c(historyList),2,usable*20/100);
        }
        layingOut=false;
        RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
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
        bool replaceQueue=false,clearQueue=false,batchAdded=false;
        bool snapshotOnly=false,snapshotLoaded=false;
    } pending;
    bool startJob(std::function<void(CollectionStore&,std::stop_token)> task,std::wstring message,bool snapshotOnly=false) {
        if(busy()) return false;
        SetWindowTextW(c(status),message.c_str());
        pending={};
        pending.snapshotOnly=snapshotOnly;
        worker=std::jthread([this,task=std::move(task)](std::stop_token stop) {
            try {
                if(!store) store=std::make_shared<CollectionStore>(database.empty()?defaultDatabasePath():database);
                task(*store,stop);
                pending.snapshot=store->snapshot();
                pending.snapshotLoaded=true;
                pending.history=store->history();
                if(!pending.snapshotOnly) {
                    pending.presets=store->presets();
                }
                if(pending.message.empty()) pending.message=L"Готово";
            } catch(const std::exception& e) {
                pending.message=wide(e.what());
                if(store) {
                    try {
                        pending.snapshot=store->snapshot();
                        pending.snapshotLoaded=true;
                        pending.history=store->history();
                        if(!pending.snapshotOnly) {
                            pending.presets=store->presets();
                        }
                    } catch(...) {}
                }
            }
            PostMessageW(window,finished,0,0);
        });
        layout();
        return true;
    }
    void beginRefresh() {
        if(templateParent) {templateParent->beginRefresh();return;}
        if(busy()) {refreshPending=true;return;}
        refreshPending=false;
        startJob([](CollectionStore&,std::stop_token) {},L"Обновление художников…",true);
    }
    void replaceSnapshot(CollectionSnapshot snapshot) {
        const auto value=refreshedArtistInput(text(c(artist)),data.artists,snapshot.artists);
        const bool wasFilling=std::exchange(filling,true);
        SetWindowTextW(c(artist),value.c_str());
        filling=wasFilling;
        data=std::move(snapshot);
        const auto selected=std::ranges::find(data.artists,selectedArtistId,&Artist::id);
        if(selected==data.artists.end()) {selectedArtistId.clear();selectedArtistText.clear();}
        else selectedArtistText=selected->name;
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
        if(cancelled&&pending.batchAdded) {
            pending.replaceQueue=false;pending.batchAdded=false;
            pending.message=L"Подготовка пачки отменена";
        }
        if(pending.snapshotLoaded) replaceSnapshot(std::move(pending.snapshot));
        if(!pending.snapshotOnly) {
            presets=std::move(pending.presets);
        }
        operations=std::move(pending.history);
        if(pending.replaceQueue||(pending.previewGeneration&&pending.previewGeneration->first==selectionGeneration&&
            pending.previewGeneration->second==directoryGeneration)) preview=std::move(pending.preview);
        if(!pending.savedPresetId.empty()) naming.id=std::move(pending.savedPresetId);
        if(pending.operationDetails) SetWindowTextW(c(historyDetails),pending.operationDetails->c_str());
        if(pending.clearQueue) preview.clear();
        if(pending.batchAdded) {
            if(pending.replaceQueue&&!cancelled) batchQueued=true;
            const bool wasFilling=std::exchange(filling,true);
            SetWindowTextW(c(title),L"");SetWindowTextW(c(workNumber),L"");
            filling=wasFilling;
        }
        if(pending.files) {
            if(!pending.clearQueue&&pending.files->success) {
                for(auto& row:preview) for(const auto& [source,target]:pending.files->changes) {
                    if(CompareStringOrdinal(row.proposal.source.c_str(),-1,source.c_str(),-1,TRUE)==CSTR_EQUAL) {
                        row.proposal.source=target;break;
                    }
                }
            }
            fileResult=std::move(pending.files);
        }
        SetWindowTextW(c(status),pending.message.c_str());
        fillMetadata();
        if(!pending.snapshotOnly) {fillPresets();fillPreview();}
        fillHistory();
        updateSummary();updateExample();
        if(templateParent) templateParent->syncTemplateFromEditor();
        if(editor&&!editor->busy()) {
            editor->replaceSnapshot(data);editor->presets=presets;editor->selection=selection;editor->fillMetadata();
            if(!pending.snapshotOnly) editor->fillPresets();
            editor->updateExample();
        }
        PostMessageW(owner,changed,0,0);
        layout();
        if((!cancelled||deferredUiAction)&&(indexPending||directory!=indexed)) beginIndex();
        if(refreshPending&&!busy()) beginRefresh();
        scheduleDeferredUi();
    }
    void updateSelection(std::vector<fs::path> selected) {
        ++selectionGeneration;
        selection=std::move(selected);
        updateSummary();
        if(editor) {editor->selection=selection;editor->data=data;editor->updateExample();}
        updateExample();
    }
    void updateSummary() {
        const auto value=templateOnly?L"Шаблоны имён":L"Выбрано "+std::to_wstring(selection.size())+
            L" · в очереди "+std::to_wstring(preview.size());
        SetWindowTextW(c(summary),value.c_str());
        SetWindowTextW(c(batchPresetName),naming.name.c_str());
    }
    void fillMetadata() {
        filling=true;
        const auto artistValue=text(c(artist)),workValue=text(c(work));
        SendMessageW(c(artist),CB_RESETCONTENT,0,0);
        artistChoices.clear();
        for(const auto& a:data.artists) {
            const auto row=SendMessageW(c(artist),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(a.name.c_str()));
            if(row>=0) {
                const auto index=artistChoices.size();artistChoices.push_back(a.id);
                SendMessageW(c(artist),CB_SETITEMDATA,static_cast<WPARAM>(row),static_cast<LPARAM>(index));
            }
        }
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
    struct ArtistInput {std::wstring name;LibraryId id;};
    std::vector<ArtistInput> artistInputs() const {
        const auto value=trim(text(c(artist)));
        if(!selectedArtistId.empty()&&(selectingArtist||sameName(value,selectedArtistText))) {
            const auto selected=std::ranges::find(data.artists,selectedArtistId,&Artist::id);
            if(selected!=data.artists.end()) return {{selected->name,selected->id}};
        }
        std::vector<ArtistInput> result;
        for(const auto& name:split(value,L';')) result.push_back({name,{}});
        return result;
    }
    void selectArtist() {
        selectedArtistId.clear();selectedArtistText.clear();
        const auto row=SendMessageW(c(artist),CB_GETCURSEL,0,0);
        const auto index=row>=0?SendMessageW(c(artist),CB_GETITEMDATA,static_cast<WPARAM>(row),0):CB_ERR;
        if(index>=0&&static_cast<std::size_t>(index)<artistChoices.size()) {
            const auto selected=std::ranges::find(data.artists,artistChoices[static_cast<std::size_t>(index)],&Artist::id);
            if(selected!=data.artists.end()) {selectedArtistId=selected->id;selectedArtistText=selected->name;}
        }
        // CBN_SELCHANGE arrives before an editable combo commits the new text.
        selectingArtist=true;updateExample();selectingArtist=false;
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
        const bool wasFilling=std::exchange(filling,true);
        const auto selected=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
        const auto top=SendMessageW(c(blocks),LB_GETTOPINDEX,0,0);
        const bool wasVisible=(GetWindowLongPtrW(c(blocks),GWL_STYLE)&WS_VISIBLE)!=0;
        SendMessageW(c(blocks),WM_SETREDRAW,FALSE,0);
        SendMessageW(c(blocks),LB_RESETCONTENT,0,0);
        for(const auto& block:naming.blocks) {
            auto label=std::wstring(nameBlockLabel(block.kind));
            if(!block.text.empty()) label+=L"  "+block.text;
            if(block.digits>0) label+=L" · "+std::wstring(static_cast<std::size_t>(std::max(0,block.digits-1)),L'0')+L"1";
            SendMessageW(c(blocks),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
        if(!naming.blocks.empty()) SendMessageW(c(blocks),LB_SETCURSEL,std::clamp(selected,0,static_cast<int>(naming.blocks.size())-1),0);
        SendMessageW(c(blocks),LB_SETTOPINDEX,top,0);
        SendMessageW(c(blocks),WM_SETREDRAW,TRUE,0);
        if(!wasVisible) ShowWindow(c(blocks),SW_HIDE);
        InvalidateRect(c(blocks),nullptr,TRUE);
        filling=wasFilling;
        updateExample();
    }
    void fillGlobals() {
        const bool wasFilling=std::exchange(filling,true);
        SetWindowTextW(c(authorJoin),naming.authorSeparator.c_str());
        SetWindowTextW(c(counterStart),std::to_wstring(naming.counterStart).c_str());
        SendMessageW(c(counterScope),CB_SETCURSEL,static_cast<WPARAM>(naming.counterScope),0);
        filling=wasFilling;
        updateExample();
    }
    void selectBlock() {
        const auto index=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
        if(index<0||index>=static_cast<int>(naming.blocks.size())) return;
        const bool wasFilling=std::exchange(filling,true);
        const auto& block=naming.blocks[static_cast<std::size_t>(index)];
        SetWindowTextW(c(blockText),block.text.c_str());SetWindowTextW(c(blockDigits),std::to_wstring(block.digits).c_str());
        SetWindowTextW(c(blockPrefix),block.prefix.c_str());SetWindowTextW(c(blockSuffix),block.suffix.c_str());
        SetWindowTextW(c(blockFallback),block.fallback.c_str());
        SendMessageW(c(blockCase),CB_SETCURSEL,static_cast<WPARAM>(block.letterCase),0);
        filling=wasFilling;
        layout();
    }
    void updateExample() {
        if(!c(liveExample)) return;
        try {
            auto value=naming;
            value.authorSeparator=text(c(authorJoin));value.counterStart=integer(c(counterStart),1);
            const auto scope=SendMessageW(c(counterScope),CB_GETCURSEL,0,0);
            if(scope>=0) value.counterScope=static_cast<CounterScope>(scope);
            const auto sample=std::ranges::find_if(data.files,[&](const MediaRecord& file) {
                return std::ranges::find(selection,file.path)!=selection.end();
            });
            std::wstring result=L"Выберите файлы в каталоге";
            if(sample!=data.files.end()) {
                auto snapshot=data;auto file=*sample;
                if(!templateOnly) {
                    const auto mode=SendMessageW(c(assignment),CB_GETCURSEL,0,0);
                    Artwork workDraft;
                    const auto oldWork=std::ranges::find(snapshot.artworks,file.artworkId,&Artwork::id);
                    if(mode==3&&oldWork!=snapshot.artworks.end()) workDraft=*oldWork;
                    const auto selectedWork=SendMessageW(c(work),CB_GETCURSEL,0,0);
                    if(mode==2&&(selectedWork<0||selectedWork>=static_cast<LRESULT>(data.artworks.size()))) {
                        SetWindowTextW(c(liveExample),L"Выберите существующую работу");return;
                    }
                    if(mode==2&&selectedWork>=0&&selectedWork<static_cast<LRESULT>(data.artworks.size())) workDraft=data.artworks[static_cast<std::size_t>(selectedWork)];
                    const auto originalWorkId=workDraft.id;
                    const bool preserveExisting=mode==3&&oldWork!=snapshot.artworks.end();
                    const auto inputs=artistInputs();
                    std::vector<LibraryId> authors;
                    for(std::size_t i=0;i<inputs.size();++i) {
                        const auto byId=std::ranges::find(snapshot.artists,inputs[i].id,&Artist::id);
                        const auto found=!inputs[i].id.empty()&&byId!=snapshot.artists.end()?&*byId:namedArtist(snapshot.artists,inputs[i].name);
                        if(found) {
                            if(std::ranges::find(authors,found->id)==authors.end()) authors.push_back(found->id);
                        }
                        else {
                            Artist draft;draft.id=L"viewery-draft-artist-"+std::to_wstring(i);draft.name=inputs[i].name;draft.fileName=inputs[i].name;
                            authors.push_back(draft.id);snapshot.artists.push_back(std::move(draft));
                        }
                    }
                    workDraft.id=L"viewery-draft-work";
                    if(!inputs.empty()||mode!=2) workDraft.artists=authors;
                    std::vector<Artist> titleArtists;
                    for(const auto& id:authors) {
                        const auto found=std::ranges::find(snapshot.artists,id,&Artist::id);
                        if(found!=snapshot.artists.end()) titleArtists.push_back(*found);
                    }
                    const auto suppliedTitle=trim(text(c(title)));
                    if(!preserveExisting&&(mode!=2||!suppliedTitle.empty())) {
                        workDraft.title=suppliedTitle.empty()?inferOriginalTitle(file.path.stem().wstring(),titleArtists):suppliedTitle;
                        if(!suppliedTitle.empty()) workDraft.userTitle.clear();
                    }
                    auto number=integer(c(workNumber),0);
                    if(!preserveExisting&&(mode!=2||number>0)) {
                        if(!number) {
                            number=1;
                            for(const auto& existing:snapshot.artworks) {
                                if(authors.empty()||std::ranges::find(existing.artists,authors.front())!=existing.artists.end()) number=std::max(number,existing.number+1);
                            }
                        }
                        workDraft.number=number;
                    }
                    if(workDraft.number<=0) workDraft.number=1;
                    snapshot.artworks.push_back(workDraft);file.artworkId=workDraft.id;
                    MediaVersion version;version.id=L"viewery-draft-version";version.artworkId=workDraft.id;
                    version.number=std::max<std::int64_t>(1,integer(c(versionNumber),1));
                    if(mode==2) for(const auto& existing:snapshot.versions) {
                        if(existing.artworkId==originalWorkId) version.number=std::max(version.number,existing.number+1);
                    }
                    if(mode==3) {
                        const auto found=std::ranges::find(snapshot.versions,file.versionId,&MediaVersion::id);
                        if(found!=snapshot.versions.end()&&found->artworkId==originalWorkId) version.number=found->number;
                        else {
                            version.number=1;
                            if(!originalWorkId.empty()) for(const auto& existing:snapshot.versions) {
                                if(existing.artworkId==originalWorkId) version.number=std::max(version.number,existing.number+1);
                            }
                        }
                    }
                    snapshot.versions.push_back(version);file.versionId=version.id;
                }
                result=renderName(value,snapshot,file);
            }
            SetWindowTextW(c(liveExample),result.c_str());
        } catch(const std::exception&) {SetWindowTextW(c(liveExample),L"Проверьте параметры выбранной части имени");}
    }
    void templateChanged() {
        updateExample();
        if(templateParent) templateParent->syncTemplateFromEditor();
    }
    void applyBlock() {
        const auto index=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
        if(index<0||index>=static_cast<int>(naming.blocks.size())) return;
        auto block=naming.blocks[static_cast<std::size_t>(index)];
        block.text=text(c(blockText));block.digits=static_cast<int>(integer(c(blockDigits),0));
        if(block.digits>12) throw LibraryError("Количество знаков: от 0 до 12");
        block.prefix=text(c(blockPrefix));block.suffix=text(c(blockSuffix));block.fallback=text(c(blockFallback));
        block.letterCase=static_cast<NameCase>(SendMessageW(c(blockCase),CB_GETCURSEL,0,0));
        naming.blocks[static_cast<std::size_t>(index)]=std::move(block);
        fillBlocks();
        templateChanged();
    }
    void assignMetadata(bool addToQueue=false) {
        const auto selected=selectedRecords();
        if(selected.empty()) throw LibraryError("Select files in the catalog first");
        const auto inputs=artistInputs();
        const auto extra=parseFields(text(c(fields)));
        const auto titleValue=trim(text(c(title)));
        const auto alias=text(c(aliases));
        const auto mode=SendMessageW(c(assignment),CB_GETCURSEL,0,0);
        const auto chosen=SendMessageW(c(work),CB_GETCURSEL,0,0);
        const LibraryId chosenId=chosen>=0&&chosen<static_cast<LRESULT>(data.artworks.size())
            ?data.artworks[static_cast<std::size_t>(chosen)].id:LibraryId{};
        const auto firstNumber=integer(c(workNumber),0),firstVersion=integer(c(versionNumber),1);
        auto value=naming;
        value.authorSeparator=text(c(authorJoin));value.counterStart=integer(c(counterStart),1);
        value.counterScope=static_cast<CounterScope>(std::max<LRESULT>(0,SendMessageW(c(counterScope),CB_GETCURSEL,0,0)));
        std::vector<RenameProposal> existingQueue;
        for(const auto& row:preview) existingQueue.push_back(row.proposal);
        const auto knownArtists=data.artists;
        startJob([this,selected,inputs,extra,titleValue,alias,mode,chosenId,firstNumber,firstVersion,addToQueue,value,existingQueue,knownArtists]
                 (CollectionStore& db,std::stop_token stop) {
            auto current=db.snapshot();
            std::vector<LibraryId> artists;
            std::set<LibraryId> savedArtists;
            for(const auto& input:inputs) {
                const auto bound=input.id.empty()?namedArtist(knownArtists,input.name):nullptr;
                const auto id=!input.id.empty()?input.id:bound?bound->id:LibraryId{};
                const auto byId=std::ranges::find(current.artists,id,&Artist::id);
                if(!id.empty()&&byId==current.artists.end()) continue;
                const auto found=!id.empty()?&*byId:namedArtist(current.artists,input.name);
                Artist a;
                if(found) a=*found;
                else {a.name=input.name;a.fileName=input.name;}
                if(!a.id.empty()&&savedArtists.contains(a.id)) continue;
                if(inputs.size()==1&&!alias.empty()) a.aliases=alias;
                const auto saved=found&&(inputs.size()!=1||alias.empty()||alias==found->aliases)?a.id:db.saveArtist(a);
                savedArtists.insert(saved);
                artists.push_back(saved);
                if(!found) {a.id=saved;current.artists.push_back(std::move(a));}
            }
            std::vector<Artist> titleArtists;
            for(const auto& id:artists) {
                const auto found=std::ranges::find(current.artists,id,&Artist::id);
                if(found!=current.artists.end()) titleArtists.push_back(*found);
            }
            LibraryId common=chosenId;
            if(mode==2&&common.empty()) throw LibraryError("Choose an existing artwork");
            if(mode==2) {
                const auto found=std::ranges::find(current.artworks,common,&Artwork::id);
                if(found==current.artworks.end()) throw LibraryError("The selected artwork no longer exists");
                auto chosenWork=*found;
                if(!inputs.empty()) chosenWork.artists=artists;
                if(!titleValue.empty()) {chosenWork.title=titleValue;chosenWork.userTitle.clear();}
                if(firstNumber>0) chosenWork.number=firstNumber;
                db.saveArtwork(chosenWork);
            }
            if(mode==1) {
                Artwork a;
                a.artists=artists;a.title=titleValue.empty()?inferOriginalTitle(selected.front().path.stem().wstring(),titleArtists):titleValue;
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
                    Artwork a;a.artists=artists;a.title=titleValue.empty()?inferOriginalTitle(file.path.stem().wstring(),titleArtists):titleValue;
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
                for(const auto& [key,fieldValue]:extra) file.fields.insert_or_assign(key,fieldValue);
                db.saveFile(file);
            }
            if(stop.stop_requested()) {pending.message=L"Сохранение данных отменено";return;}
            if(addToQueue) {
                const auto snapshot=db.snapshot();
                std::vector<MediaRecord> batch;
                for(const auto& original:selected) {
                    const auto saved=std::ranges::find(snapshot.files,original.id,&MediaRecord::id);
                    if(saved==snapshot.files.end()) throw LibraryError("A selected file disappeared from the collection");
                    batch.push_back(*saved);
                }
                const auto rendered=renderNames(value,snapshot,batch);
                auto proposals=existingQueue;
                for(std::size_t i=0;i<batch.size();++i) {
                    const RenameProposal proposal{batch[i].id,batch[i].path,rendered[i],true};
                    const auto found=std::ranges::find_if(proposals,[&](const RenameProposal& item) {
                        return (!item.fileId.empty()&&item.fileId==proposal.fileId)||
                            CompareStringOrdinal(item.source.c_str(),-1,proposal.source.c_str(),-1,TRUE)==CSTR_EQUAL;
                    });
                    if(found==proposals.end()) proposals.push_back(proposal);else *found=proposal;
                }
                pending.preview=previewRenames(proposals);pending.replaceQueue=true;pending.batchAdded=true;
                pending.message=L"Пачка добавлена в очередь. Можно выбрать следующую.";
            } else pending.message=L"Данные сохранены";
        },addToQueue?L"Подготовка пачки…":L"Сохранение данных…");
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
        assignMetadata(true);
    }
    void revalidateQueue() {
        std::vector<RenameProposal> proposals;
        for(const auto& row:preview) proposals.push_back(row.proposal);
        preview=previewRenames(proposals);fillPreview();
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
        updateSummary();
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
        std::vector<RenameProposal> proposals;
        std::vector<RenameProposal> excluded;
        for(const auto& row:preview) if(row.proposal.enabled) {
            proposals.push_back(row.proposal);
        } else excluded.push_back(row.proposal);
        if(proposals.empty()) throw LibraryError("Добавьте файлы в очередь переименования");
        const auto verified=previewRenames(proposals);
        if(std::ranges::any_of(verified,[](const RenamePreview& row){return !row.ready;})) {
            preview=previewRenames([&]{std::vector<RenameProposal> all=proposals;all.insert(all.end(),excluded.begin(),excluded.end());return all;}());
            fillPreview();throw LibraryError("В очереди есть конфликты. Исправьте имена или исключите строки.");
        }
        const auto prompt=L"Переименовать все "+std::to_wstring(proposals.size())+L" файлов из очереди одной операцией?";
        if(MessageBoxW(window,prompt.c_str(),L"Переименование",MB_YESNO|MB_ICONQUESTION)!=IDYES) return;
        before();
        startJob([this,proposals,excluded](CollectionStore& db,std::stop_token stop) {
            pending.files=db.rename(proposals,stop);
            pending.message=pending.files->message;
            if(pending.files->success) {pending.preview=previewRenames(excluded);pending.replaceQueue=true;}
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
        if(id==closePanel) {close();return;}
        if(filling) return;
        if(notification==CBN_CLOSEUP) {layout();return;}
        if(id>=tabAssign&&id<=tabHistory) {
            if(id==tabTemplate&&!templateOnly) {openEditor();return;}
            if(templateOnly) return;
            tab=id-tabAssign;scrollOffset=0;
            layout();return;
        }
        if(id==previewSearch&&notification==EN_CHANGE) {fillPreview();return;}
        if(id==cancelTask&&notification==BN_CLICKED) {cancelJobs();return;}
        if((id==dbBackup||id==metaExport||id==metaImport)&&notification==BN_CLICKED&&postponeUi(id)) return;
        if(busy()) return;
        if(id==artist&&notification==CBN_SELCHANGE) {selectArtist();return;}
        if((id==assignment&&notification==CBN_SELCHANGE)||(id==title&&notification==EN_CHANGE)||
            (id==artist&&notification==CBN_EDITCHANGE)) {
            if(id==artist) {selectedArtistId.clear();selectedArtistText.clear();}
            if(id==assignment) layout();
            updateExample();return;
        }
        if((id==blockText||id==blockDigits||id==blockPrefix||id==blockSuffix||id==blockFallback)&&notification==EN_CHANGE) {
            applyBlock();return;
        }
        if(id==blockCase&&notification==CBN_SELCHANGE) {applyBlock();return;}
        if(((id==authorJoin||id==counterStart)&&notification==EN_CHANGE)||(id==counterScope&&notification==CBN_SELCHANGE)) {
            templateChanged();return;
        }
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
                naming=parseTemplate(utf8(saved.json));naming.id=saved.id;fillBlocks();fillGlobals();selectBlock();templateChanged();
            }
            return;
        }
        if(id==blocks&&notification==LBN_SELCHANGE) {selectBlock();return;}
        if(notification!=BN_CLICKED) return;
        switch(id) {
        case RenameStudio::artistsCommandId:
            if(!templateOnly) PostMessageW(owner,WM_COMMAND,MAKEWPARAM(RenameStudio::artistsCommandId,BN_CLICKED),
                reinterpret_cast<LPARAM>(c(RenameStudio::artistsCommandId)));
            break;
        case dataAdvanced:dataExpanded=!dataExpanded;layout();break;
        case queueClear:preview.clear();fillPreview();SetWindowTextW(c(status),L"Очередь очищена. Файлы не изменены.");break;
        case queueRemove: {
            const int row=ListView_GetNextItem(c(previewList),-1,LVNI_SELECTED);
            if(row>=0&&row<static_cast<int>(shown.size())) preview.erase(preview.begin()+static_cast<std::ptrdiff_t>(shown[static_cast<std::size_t>(row)]));
            revalidateQueue();break;
        }
        case advancedToggle:advanced=!advanced;layout();break;
        case presetMenu: {
            RECT bounds{};GetWindowRect(c(presetMenu),&bounds);
            HMENU menu=CreatePopupMenu();
            const auto add=[&](int commandId,const wchar_t* label){AppendMenuW(menu,MF_STRING,commandId,label);};
            add(newPreset,L"Новый шаблон");add(presetDuplicate,L"Сохранить копию");add(presetDelete,L"Удалить пресет");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr);add(presetImport,L"Импорт JSON…");add(presetExport,L"Экспорт JSON…");
            const int chosen=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,bounds.right,bounds.bottom,0,window,nullptr);
            DestroyMenu(menu);
            if(chosen) command(chosen,BN_CLICKED);
            break;
        }
        case newPreset:
            naming=defaultArtistTemplate();naming.id.clear();naming.name=L"Новый шаблон";
            SetWindowTextW(c(preset),naming.name.c_str());fillBlocks();fillGlobals();selectBlock();templateChanged();break;
        case assign:assignMetadata();break;
        case presetSave:savePreset(false);break;
        case presetDuplicate:savePreset(true);break;
        case presetDelete: {
            const auto presetId=naming.id;
            if(presetId.empty()) break;
            startJob([presetId](CollectionStore& db,std::stop_token){db.deletePreset(presetId);},L"Удаление пресета…");
            naming=defaultArtistTemplate();fillBlocks();fillGlobals();
            SetWindowTextW(c(preset),naming.name.c_str());selectBlock();templateChanged();break;
        }
        case presetImport: {
            const auto path=pick(window,false,{});
            if(!path) break;
            std::ifstream input(*path,std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(input)),{});
            naming=parseTemplate(contents);naming.id.clear();
            SetWindowTextW(c(preset),naming.name.c_str());fillBlocks();fillGlobals();selectBlock();templateChanged();break;
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
            if(block.kind==NameBlockKind::workNumber||block.kind==NameBlockKind::counter) block.digits=3;
            if(block.kind==NameBlockKind::version) {block.digits=2;block.prefix=L"v";}
            if(block.kind==NameBlockKind::date) block.text=L"%Y-%m-%d";
            naming.blocks.push_back(block);fillBlocks();
            SendMessageW(c(blocks),LB_SETCURSEL,naming.blocks.size()-1,0);selectBlock();templateChanged();break;
        }
        case blockApply:applyBlock();break;
        case blockRemove: {
            const auto index=SendMessageW(c(blocks),LB_GETCURSEL,0,0);
            if(index>=0&&index<static_cast<LRESULT>(naming.blocks.size())) naming.blocks.erase(naming.blocks.begin()+index);
            fillBlocks();selectBlock();templateChanged();break;
        }
        case blockUp:case blockDown: {
            const auto index=static_cast<int>(SendMessageW(c(blocks),LB_GETCURSEL,0,0));
            const auto next=index+(id==blockUp?-1:1);
            if(index>=0&&next>=0&&next<static_cast<int>(naming.blocks.size())) {
                std::swap(naming.blocks[static_cast<std::size_t>(index)],naming.blocks[static_cast<std::size_t>(next)]);
                fillBlocks();SendMessageW(c(blocks),LB_SETCURSEL,next,0);selectBlock();templateChanged();
            }
            break;
        }
        case makePreview:
            if(templateOnly) {
                if(templateParent) templateParent->syncTemplateFromEditor();
                close();
            } else createPreview();
            break;
        case editName: {
            const int row=ListView_GetNextItem(c(previewList),-1,LVNI_SELECTED);
            if(row<0||row>=static_cast<int>(shown.size())) break;
            auto proposals=std::vector<RenameProposal>{};
            const auto changedRow=shown[static_cast<std::size_t>(row)];
            for(std::size_t i=0;i<preview.size();++i) {
                auto proposal=preview[i].proposal;
                if(i==changedRow) proposal.newName=text(c(newName));
                proposals.push_back(std::move(proposal));
            }
            startJob([this,proposals](CollectionStore&,std::stop_token){
                pending.preview=previewRenames(proposals);pending.replaceQueue=true;},
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
    static LRESULT CALLBACK inputProc(HWND child,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& self=*reinterpret_cast<Impl*>(data);
        if(msg==WM_KEYDOWN&&w==VK_ESCAPE) {
            if(self.dropdownOpen()) return DefSubclassProc(child,msg,w,l);
            self.close();return 0;
        }
        if(msg==WM_MOUSEWHEEL) {
            if(self.dropdownOpen()) return DefSubclassProc(child,msg,w,l);
            wchar_t name[30]{};GetClassNameW(child,name,30);
            if(wcscmp(name,L"SysListView32")==0) return DefSubclassProc(child,msg,w,l);
            if(wcscmp(name,L"ListBox")==0||wcscmp(name,L"Edit")==0) {
                SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE};
                if(GetScrollInfo(child,SB_VERT,&info)&&info.nMax>=static_cast<int>(info.nPage)&&info.nPage>0) {
                    return DefSubclassProc(child,msg,w,l);
                }
            }
            return SendMessageW(self.viewport,WM_MOUSEWHEEL,w,l);
        }
        if(msg==WM_KEYDOWN&&GetDlgCtrlID(child)==blocks&&!self.busy()) {
            if(w==VK_DELETE) {self.command(blockRemove,BN_CLICKED);return 0;}
            if((w==VK_UP||w==VK_DOWN)&&(GetKeyState(VK_CONTROL)&0x8000)) {
                self.command(w==VK_UP?blockUp:blockDown,BN_CLICKED);return 0;
            }
        }
        return DefSubclassProc(child,msg,w,l);
    }
    static LRESULT CALLBACK blocksProc(HWND child,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& self=*reinterpret_cast<Impl*>(data);
        if(msg==WM_LBUTTONDOWN&&!self.busy()) {
            const auto hit=SendMessageW(child,LB_ITEMFROMPOINT,0,l);
            self.dragBlock=HIWORD(hit)==0?static_cast<int>(LOWORD(hit)):-1;
            self.dragOrigin={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            self.dragging=false;
        } else if(msg==WM_MOUSEMOVE&&self.dragBlock>=0&&(w&MK_LBUTTON)&&!self.busy()) {
            if(!self.dragging&&(abs(GET_X_LPARAM(l)-self.dragOrigin.x)>=GetSystemMetrics(SM_CXDRAG)||
                abs(GET_Y_LPARAM(l)-self.dragOrigin.y)>=GetSystemMetrics(SM_CYDRAG))) {
                self.dragging=true;SetCapture(child);
            }
            if(self.dragging) {SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));return 0;}
        } else if(msg==WM_LBUTTONUP&&self.dragBlock>=0&&!self.busy()) {
            const int from=std::exchange(self.dragBlock,-1);
            const bool wasDragging=std::exchange(self.dragging,false);
            if(wasDragging) {
                const auto hit=SendMessageW(child,LB_ITEMFROMPOINT,0,l);
                const int to=static_cast<int>(LOWORD(hit));
                ReleaseCapture();
                if(HIWORD(hit)==0&&from!=to&&from>=0&&from<static_cast<int>(self.naming.blocks.size())&&to<static_cast<int>(self.naming.blocks.size())) {
                    auto block=std::move(self.naming.blocks[static_cast<std::size_t>(from)]);
                    self.naming.blocks.erase(self.naming.blocks.begin()+from);
                    self.naming.blocks.insert(self.naming.blocks.begin()+to,std::move(block));
                    self.fillBlocks();SendMessageW(child,LB_SETCURSEL,to,0);self.selectBlock();self.templateChanged();
                }
                return 0;
            }
        } else if(msg==WM_CAPTURECHANGED&&reinterpret_cast<HWND>(l)!=child) {
            self.dragBlock=-1;self.dragging=false;
        }
        return DefSubclassProc(child,msg,w,l);
    }
    LRESULT dispatch(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
        try {
            switch(msg) {
            case WM_VSCROLL: {
                SCROLLINFO scroll{sizeof(scroll),SIF_TRACKPOS};
                GetScrollInfo(viewport,SB_VERT,&scroll);
                const int action=LOWORD(w);
                int next=scrollOffset;
                if(action==SB_LINEUP) next-=32;
                if(action==SB_LINEDOWN) next+=32;
                if(action==SB_PAGEUP) next-=180;
                if(action==SB_PAGEDOWN) next+=180;
                if(action==SB_TOP) next=0;
                if(action==SB_BOTTOM) next=scrollLimit;
                if(action==SB_THUMBTRACK||action==SB_THUMBPOSITION) next=scroll.nTrackPos;
                scrollTo(next);return 0;
            }
            case WM_MOUSEWHEEL:
                if(hwnd==window&&!dropdownOpen()) {
                    const POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                    const HWND hovered=WindowFromPoint(point);
                    if(hovered&&hovered!=viewport&&hovered!=content&&IsChild(window,hovered)) {
                        return SendMessageW(hovered,msg,w,l);
                    }
                }
                scrollTo(scrollOffset-MulDiv(GET_WHEEL_DELTA_WPARAM(w),45,WHEEL_DELTA));return 0;
            case WM_SIZE:if(hwnd==window) layout();return 0;
            case WM_KEYDOWN:if(w==VK_ESCAPE) {close();return 0;}break;
            case finished:finishJob();return 0;
            case validateQueueMessage:if(!busy()) revalidateQueue();return 0;
            case deferredUiMessage:
                deferredUiPosted=false;
                if(deferredUiAction&&!busy()&&!indexPending&&directory==indexed) {
                    const int action=std::exchange(deferredUiAction,0);
                    command(action,BN_CLICKED);
                }
                return 0;
            case WM_COMMAND:command(LOWORD(w),HIWORD(w));return 0;
            case WM_ERASEBKGND: {
                RECT client{};GetClientRect(hwnd,&client);fill(reinterpret_cast<HDC>(w),client,bg);return 1;
            }
            case WM_PAINT: {
                PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);fill(dc,paint.rcPaint,bg);EndPaint(hwnd,&paint);return 0;
            }
            case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX: {
                HDC dc=reinterpret_cast<HDC>(w);
                SetTextColor(dc,fg);SetBkColor(dc,msg==WM_CTLCOLORSTATIC?bg:inputColor);
                return reinterpret_cast<LRESULT>(msg==WM_CTLCOLORSTATIC?brush:editBrush);
            }
            case WM_MEASUREITEM: {
                auto& measure=*reinterpret_cast<MEASUREITEMSTRUCT*>(l);
                if(measure.CtlType==ODT_LISTBOX&&measure.CtlID==blocks) {measure.itemHeight=dp(32);return TRUE;}
                if(measure.CtlType==ODT_COMBOBOX) {measure.itemHeight=dp(26);return TRUE;}
                break;
            }
            case WM_DRAWITEM: {
                auto& draw=*reinterpret_cast<DRAWITEMSTRUCT*>(l);
                if(draw.CtlID==liveExample) {
                    fill(draw.hDC,draw.rcItem,bg);
                    const auto old=SelectObject(draw.hDC,font?font:GetStockObject(DEFAULT_GUI_FONT));
                    SetBkMode(draw.hDC,TRANSPARENT);SetTextColor(draw.hDC,fg);
                    const auto value=text(draw.hwndItem);
                    RECT bounds=draw.rcItem;
                    DrawTextW(draw.hDC,value.c_str(),static_cast<int>(value.size()),&bounds,
                        DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                    SelectObject(draw.hDC,old);return TRUE;
                }
                if(draw.CtlType==ODT_COMBOBOX) {
                    const bool selected=(draw.itemState&ODS_SELECTED)!=0;
                    fill(draw.hDC,draw.rcItem,selected?RGB(54,46,86):inputColor);
                    const auto index=draw.itemID==static_cast<UINT>(-1)
                        ?SendMessageW(draw.hwndItem,CB_GETCURSEL,0,0):static_cast<LRESULT>(draw.itemID);
                    const auto count=SendMessageW(draw.hwndItem,CB_GETCOUNT,0,0);
                    std::wstring value;
                    if(index>=0&&index<count) {
                        const auto length=SendMessageW(draw.hwndItem,CB_GETLBTEXTLEN,static_cast<WPARAM>(index),0);
                        if(length>=0&&length<=32768) {
                            value.resize(static_cast<std::size_t>(length)+1,L'\0');
                            const auto copied=SendMessageW(draw.hwndItem,CB_GETLBTEXT,static_cast<WPARAM>(index),reinterpret_cast<LPARAM>(value.data()));
                            value.resize(copied>=0?static_cast<std::size_t>(copied):0);
                        }
                    }
                    const auto old=SelectObject(draw.hDC,font?font:GetStockObject(DEFAULT_GUI_FONT));
                    SetBkMode(draw.hDC,TRANSPARENT);
                    SetTextColor(draw.hDC,(draw.itemState&ODS_DISABLED)?muted:selected?accent:fg);
                    RECT bounds=draw.rcItem;bounds.left+=dp(7);bounds.right-=dp(7);
                    DrawTextW(draw.hDC,value.c_str(),static_cast<int>(value.size()),&bounds,
                        DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                    SelectObject(draw.hDC,old);return TRUE;
                }
                if(draw.CtlType==ODT_LISTBOX&&draw.CtlID==blocks) {
                    const bool selected=(draw.itemState&ODS_SELECTED)!=0;
                    fill(draw.hDC,draw.rcItem,selected?RGB(54,46,86):inputColor);
                    if(draw.itemID==static_cast<UINT>(-1)) return TRUE;
                    const int count=static_cast<int>(SendMessageW(draw.hwndItem,LB_GETTEXTLEN,draw.itemID,0));
                    if(count<0) return TRUE;
                    std::wstring value(static_cast<std::size_t>(count)+1,L'\0');
                    SendMessageW(draw.hwndItem,LB_GETTEXT,draw.itemID,reinterpret_cast<LPARAM>(value.data()));
                    const auto old=SelectObject(draw.hDC,font);
                    SetBkMode(draw.hDC,TRANSPARENT);SetTextColor(draw.hDC,selected?accent:fg);
                    RECT bounds=draw.rcItem;bounds.left+=dp(10);bounds.right-=dp(10);
                    DrawTextW(draw.hDC,value.c_str(),count,&bounds,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                    SelectObject(draw.hDC,old);return TRUE;
                }
                const bool active=(draw.CtlID==static_cast<UINT>(tabAssign+tab));
                fill(draw.hDC,draw.rcItem,active?RGB(42,37,69):inputColor);
                const auto old=SelectObject(draw.hDC,draw.CtlID==RenameStudio::artistsCommandId&&iconFont?iconFont:font);
                SetBkMode(draw.hDC,TRANSPARENT);
                SetTextColor(draw.hDC,(draw.itemState&ODS_DISABLED)?muted:active?accent:fg);
                auto value=text(draw.hwndItem);
                if(draw.CtlID==RenameStudio::artistsCommandId) value=L"\xE77B";
                RECT bounds=draw.rcItem;bounds.left+=dp(5);bounds.right-=dp(5);
                DrawTextW(draw.hDC,value.c_str(),static_cast<int>(value.size()),&bounds,
                          DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
                SelectObject(draw.hDC,old);return TRUE;
            }
            case WM_NOTIFY: {
                const auto* header=reinterpret_cast<const NMHDR*>(l);
                if(header->code==TTN_GETDISPINFOW) {
                    auto& info=*reinterpret_cast<NMTTDISPINFOW*>(l);
                    const HWND control=reinterpret_cast<HWND>(header->idFrom);
                    if(control==c(blockUp)) tooltipText=L"Переместить часть выше · Ctrl+↑";
                    else if(control==c(blockDown)) tooltipText=L"Переместить часть ниже · Ctrl+↓";
                    else if(control==c(closePanel)) tooltipText=L"Закрыть панель · Esc";
                    else if(control==c(RenameStudio::artistsCommandId)) tooltipText=L"База художников · добавить, изменить или удалить";
                    else if(control==c(blocks)) {
                        POINT point{};GetCursorPos(&point);ScreenToClient(control,&point);
                        const auto hit=SendMessageW(control,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));
                        const auto count=HIWORD(hit)==0?SendMessageW(control,LB_GETTEXTLEN,LOWORD(hit),0):LB_ERR;
                        tooltipText.clear();
                        if(count>=0) {tooltipText.resize(static_cast<std::size_t>(count)+1);SendMessageW(control,LB_GETTEXT,LOWORD(hit),reinterpret_cast<LPARAM>(tooltipText.data()));}
                    } else tooltipText=text(control);
                    info.lpszText=tooltipText.data();return 0;
                }
                if(header->idFrom==previewList&&header->code==LVN_ITEMCHANGED&&!filling) {
                    const auto& event=*reinterpret_cast<const NMLISTVIEW*>(l);
                    if(event.iItem>=0&&event.iItem<static_cast<int>(shown.size())) {
                        auto& row=preview[shown[static_cast<std::size_t>(event.iItem)]];
                        const bool enabled=ListView_GetCheckState(c(previewList),event.iItem)!=FALSE;
                        const bool changedState=row.proposal.enabled!=enabled;
                        row.proposal.enabled=enabled;
                        if(event.uNewState&LVIS_SELECTED) SetWindowTextW(c(newName),row.proposal.newName.c_str());
                        if(changedState&&!busy()) PostMessageW(window,validateQueueMessage,0,0);
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
    const bool styleChanged=impl_->dpi!=dpi||impl_->font!=font;
    const bool boundsChanged=!impl_->hasShownBounds||!EqualRect(&bounds,&impl_->shownBounds);
    const bool visible=IsWindowVisible(impl_->window)!=FALSE;
    if(!styleChanged&&!boundsChanged&&visible) return;
    if(styleChanged) impl_->setFont(font,dpi);
    impl_->shownBounds=bounds;impl_->hasShownBounds=true;
    SetWindowPos(impl_->window,HWND_TOP,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                 SWP_NOACTIVATE|SWP_SHOWWINDOW);
    impl_->layout();
}
void RenameStudio::hide() {ShowWindow(impl_->window,SW_HIDE);}
void RenameStudio::openTemplates() {
    SendMessageW(impl_->window,WM_COMMAND,MAKEWPARAM(tabTemplate,BN_CLICKED),reinterpret_cast<LPARAM>(impl_->c(tabTemplate)));
}
HWND RenameStudio::templatesWindow() const {return impl_->editorHost;}
bool RenameStudio::processMessage(MSG& message) {
    if(!impl_->editorHost||!impl_->editor||!IsWindowVisible(impl_->editorHost)||
        (message.hwnd!=impl_->editorHost&&!IsChild(impl_->editorHost,message.hwnd))) return false;
    if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE&&!impl_->editor->dropdownOpen()) {
        ShowWindow(impl_->editorHost,SW_HIDE);return true;
    }
    return message.message==WM_KEYDOWN&&message.wParam==VK_TAB&&IsDialogMessageW(impl_->editor->window,&message);
}
void RenameStudio::collectionAction(CollectionAction action) {
    const int id=action==CollectionAction::backup?dbBackup:action==CollectionAction::exportMetadata?metaExport:metaImport;
    SendMessageW(impl_->window,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(impl_->c(id)));
}
std::size_t RenameStudio::queuedFileCount() const {return impl_->preview.size();}
bool RenameStudio::takeBatchQueued() {return std::exchange(impl_->batchQueued,false);}
bool RenameStudio::busy() const {return impl_->busy();}
void RenameStudio::cancel() {impl_->cancelJobs();}
void RenameStudio::setFiles(std::vector<fs::path> directory,std::vector<fs::path> selected) {
    const bool changed=directory!=impl_->directory;
    if(changed) {
        ++impl_->directoryGeneration;
    }
    impl_->directory=std::move(directory);
    if(selected!=impl_->selection) impl_->updateSelection(std::move(selected));
    if(changed||(!impl_->busy()&&!impl_->store)) impl_->beginIndex();
}
void RenameStudio::refreshCollection() {impl_->beginRefresh();}
const CollectionSnapshot& RenameStudio::snapshot() const {return impl_->data;}
std::optional<RenameResult> RenameStudio::takeFileChanges() {return std::exchange(impl_->fileResult,std::nullopt);}
}
