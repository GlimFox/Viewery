#include "shell_selection.hpp"
#include <shlobj.h>
#include <map>
#include <set>
#include <stdexcept>
#include <limits>

namespace viewery {
namespace {
std::wstring pathKey(const std::filesystem::path& path) {
    const auto value=path.lexically_normal().wstring();
    if(value.empty()) return {};
    std::wstring key(value.size(),L'\0');
    if(!LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),
        key.data(),static_cast<int>(key.size()),nullptr,nullptr,0)) return value;
    return key;
}
struct Pidl {
    PIDLIST_ABSOLUTE value=nullptr;
    ~Pidl(){CoTaskMemFree(value);}
};
}
std::vector<ExplorerSelection> planExplorerSelection(const std::vector<std::filesystem::path>& files) {
    std::vector<ExplorerSelection> result;
    std::map<std::wstring,std::size_t> directories;
    std::set<std::wstring> seen;
    for(const auto& input:files) {
        if(input.empty()||!input.is_absolute()||input.filename().empty()) throw std::invalid_argument("Expected an absolute file path");
        const auto file=input.lexically_normal();
        if(!seen.insert(pathKey(file)).second) continue;
        const auto folder=file.parent_path();
        const auto [entry,inserted]=directories.emplace(pathKey(folder),result.size());
        if(inserted) result.push_back({folder,{}});
        result[entry->second].files.push_back(file);
    }
    return result;
}
HRESULT revealInExplorer(const std::vector<std::filesystem::path>& files) {
    const auto plan=planExplorerSelection(files);
    HRESULT failure=S_OK;
    for(const auto& group:plan) {
        if(group.files.size()>std::numeric_limits<UINT>::max()) return E_INVALIDARG;
        Pidl folder;
        auto result=SHParseDisplayName(group.directory.c_str(),nullptr,&folder.value,0,nullptr);
        if(FAILED(result)) {failure=result;continue;}
        std::vector<PIDLIST_ABSOLUTE> absolute;
        std::vector<PCUITEMID_CHILD> relative;
        for(const auto& file:group.files) {
            PIDLIST_ABSOLUTE item=nullptr;
            result=SHParseDisplayName(file.c_str(),nullptr,&item,0,nullptr);
            if(FAILED(result)) {failure=result;continue;}
            absolute.push_back(item);relative.push_back(ILFindLastID(item));
        }
        if(!relative.empty()) {
            result=SHOpenFolderAndSelectItems(folder.value,static_cast<UINT>(relative.size()),relative.data(),0);
            if(FAILED(result)) failure=result;
        }
        for(auto* item:absolute) CoTaskMemFree(item);
    }
    return failure;
}
}
